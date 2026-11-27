"""Tests of what a Page's requests say of themselves: fetch()'s credentials, mode, referrer, referrer policy and keepalive,
and a classic script's crossorigin and referrerpolicy, through Session._open_page() and AsyncSession._open_page(), which
stay private until M10.

Two local servers stand for the page's origin and another, on the same host, so the Session's cookies go to both and a
request to the other is same-site but cross-origin. Where a caller can ask for the same request, the lines that the Page
sent are checked against the caller's, as the Session's Request Kinds make both. Each test needs a native library with
the Page program beside it.
"""

import http.server
import json
import threading
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page, PageNavigation
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_HTML = "text/html; charset=utf-8"


@dataclass
class _Route:
    """What the server answers for one path. With `hold`, it answers only once that is set."""

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: threading.Event | None = None


class _Server:
    """A local server that records the header lines of each request it gets, in order, by path.

    An OPTIONS request is recorded as "OPTIONS PATH". A path without a route gets a 404.
    """

    def __init__(self) -> None:
        self.routes: dict[str, _Route] = {}
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.requested = threading.Condition()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                if self.command == "POST":
                    self.rfile.read(int(self.headers.get("Content-Length", "0")))
                path = f"OPTIONS {self.path}" if self.command == "OPTIONS" else self.path
                with server.requested:
                    server.requests.append((path, list(self.headers.items())))
                    server.requested.notify_all()
                route = server.routes.get(path, _Route(b"not here", 404, [("Content-Type", "text/plain")]))
                if route.hold is not None:
                    route.hold.wait(_TIMEOUT_SECONDS)
                self.send_response(route.status)
                for name, value in route.headers:
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(route.body)))
                self.end_headers()
                self.wfile.write(route.body)

            def do_POST(self) -> None:
                self.do_GET()

            def do_OPTIONS(self) -> None:
                self.do_GET()

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"

    def url(self, path: str) -> str:
        return f"{self.origin}{path}"

    def page(self, path: str, markup: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = _Route(markup.encode(), headers=[("Content-Type", _HTML), *(headers or [])])
        return self.url(path)

    def text(self, path: str, body: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = _Route(body.encode(), headers=[("Content-Type", "text/plain"), *(headers or [])])
        return self.url(path)

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        """Return the header lines of each request for `path`, in the order they came."""
        with self.requested:
            return [headers for requested, headers in self.requests if requested == path]

    def wait_for(self, path: str, timeout: float = _TIMEOUT_SECONDS) -> bool:
        """Return whether a request for `path` has come, waiting up to `timeout` for one."""
        with self.requested:
            return self.requested.wait_for(lambda: any(requested == path for requested, _ in self.requests), timeout)

    def __enter__(self) -> "_Server":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_Server]:
    with _Server() as page_server:
        yield page_server


@pytest.fixture
def other() -> Iterator[_Server]:
    with _Server() as other_server:
        yield other_server


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session through each API in turn, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _outcome(page: Page, call: str) -> Any:
    """Return what the fetch() call `call` gives a script: its response's type and status, or the error it rejects with."""
    return page.evaluate(
        f"{call}.then(response => [response.type, response.status], error => String(error))", timeout=_TIMEOUT_SECONDS
    )


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


def test_a_fetchs_credentials_mode_says_whether_the_sessions_cookies_go(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    url = server.page("/", "<p>credentials</p>", [("Set-Cookie", "who=mitto")])
    other.text("/any", "any", [("Access-Control-Allow-Origin", "*")])
    other.text(
        "/named",
        "named",
        [("Access-Control-Allow-Origin", server.origin), ("Access-Control-Allow-Credentials", "true")],
    )
    server.text("/omitted", "omitted")
    # Access-Control-Allow-Credentials counts only for a request with credentials, even twice.
    other.text(
        "/doubled",
        "doubled",
        [
            ("Access-Control-Allow-Origin", "*"),
            ("Access-Control-Allow-Credentials", "true"),
            ("Access-Control-Allow-Credentials", "true"),
        ],
    )

    with _open_page(session, url) as page:
        outcomes = [
            _outcome(page, f"fetch('{other.url('/any')}')"),
            _outcome(page, f"fetch('{other.url('/named')}', {{credentials: 'include'}})"),
            # A response that allows any origin does not pass for a request with credentials.
            _outcome(page, f"fetch('{other.url('/any')}', {{credentials: 'include'}})"),
            _outcome(page, "fetch('/omitted', {credentials: 'omit'})"),
            _outcome(page, f"fetch('{other.url('/doubled')}')"),
        ]

    assert outcomes == [["cors", 200], ["cors", 200], "TypeError: Failed to fetch", ["basic", 200], ["cors", 200]]
    without, with_credentials = other.headers_of("/any")
    assert _value(without, "Cookie") is None
    assert _value(with_credentials, "Cookie") == "who=mitto"
    assert _value(other.headers_of("/named")[0], "Cookie") == "who=mitto"
    assert _value(server.headers_of("/omitted")[0], "Cookie") is None
    # Each goes as a caller's fetch with the same credentials mode from the document goes.
    session.get(other.url("/named"), kind="fetch", initiator=url, credentials="include")
    page_lines, caller_lines = other.headers_of("/named")
    assert page_lines == caller_lines


def test_an_exposed_star_shows_every_header_only_to_a_fetch_without_credentials(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    # With credentials, "*" in Access-Control-Expose-Headers is a name like any other, as Blink reads it.
    url = server.page("/", "<p>exposed</p>")
    other.text(
        "/exposed",
        "exposed",
        [
            ("Access-Control-Allow-Origin", server.origin),
            ("Access-Control-Allow-Credentials", "true"),
            ("Access-Control-Expose-Headers", "*"),
            ("X-Shown", "1"),
        ],
    )

    with _open_page(session, url) as page:
        shown = page.evaluate(
            f"Promise.all(['omit', 'include'].map(credentials => fetch('{other.url('/exposed')}', {{credentials}})"
            ".then(response => response.headers.get('x-shown'))))",
            timeout=_TIMEOUT_SECONDS,
        )

    assert shown == ["1", None]


def test_a_no_cors_fetch_gives_an_opaque_response_and_sends_only_safelisted_lines(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    url = server.page("/", "<p>no-cors</p>")
    other.text("/closed", "closed", [("X-Hidden", "1")])

    with _open_page(session, url) as page:
        opaque = page.evaluate(
            f"fetch('{other.url('/closed')}', {{mode: 'no-cors', headers: {{'X-Custom': '1', 'Accept-Language': 'fr',"
            " 'Content-Type': 'application/json'}}).then(async response => [response.type, response.status,"
            " response.ok, response.statusText, response.url, response.headers.get('x-hidden'), await response.text()])",
            timeout=_TIMEOUT_SECONDS,
        )
        refused = page.evaluate(
            f"Promise.all([fetch('{other.url('/closed')}', {{mode: 'no-cors', method: 'PUT'}}).then(() => 'sent', String),"
            " Promise.resolve().then(() => new Request('/x', {mode: 'no-cors', method: 'PUT'})).catch(String)])",
            timeout=_TIMEOUT_SECONDS,
        )

    assert opaque == ["opaque", 0, False, "", "", None, ""]
    assert refused == [
        "TypeError: Failed to execute 'fetch' on 'Window': 'PUT' is unsupported in no-cors mode.",
        "TypeError: Failed to construct 'Request': 'PUT' is unsupported in no-cors mode.",
    ]
    # No preflight goes, and the request takes only Accept-Language of the script's lines.
    assert other.headers_of("OPTIONS /closed") == []
    (lines,) = other.headers_of("/closed")
    assert _value(lines, "Sec-Fetch-Mode") == "no-cors"
    assert _value(lines, "Accept-Language") == "fr"
    assert _value(lines, "X-Custom") is None
    assert _value(lines, "Content-Type") is None
    assert _value(lines, "Origin") is None


def test_a_fetchs_referrer_and_referrer_policy_set_its_referer(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    url = server.page("/page?x=1", "<p>referrer</p>")
    for path in ("/document", "/none", "/chosen", "/origin", "/elsewhere", "/blob"):
        server.text(path, path)
    other.text("/unsafe", "unsafe", [("Access-Control-Allow-Origin", "*")])
    other.text("/strict", "strict", [("Access-Control-Allow-Origin", "*")])

    with _open_page(session, url) as page:
        outcomes = [
            _outcome(page, "fetch('/document')"),
            _outcome(page, "fetch('/none', {referrer: ''})"),
            _outcome(page, "fetch('/chosen', {referrer: '/other?y=2'})"),
            _outcome(page, "fetch('/origin', {referrerPolicy: 'origin'})"),
            # A referrer of another origin leaves the document's.
            _outcome(page, "fetch('/elsewhere', {referrer: 'http://elsewhere.test/'})"),
            # A blob: URL is of the document's origin, and Referrer Policy gives none for it.
            _outcome(page, "fetch('/blob', {referrer: URL.createObjectURL(new Blob(['x']))})"),
            _outcome(page, f"fetch('{other.url('/unsafe')}', {{referrerPolicy: 'unsafe-url'}})"),
            _outcome(page, f"fetch('{other.url('/strict')}')"),
        ]
        invalid = page.evaluate("try { new Request('/x', {referrer: 'http://['}); 'made' } catch (e) { String(e) }")

    assert outcomes[:6] == [["basic", 200]] * 6
    assert outcomes[6:] == [["cors", 200]] * 2
    assert invalid == "TypeError: Failed to construct 'Request': Referrer 'http://[' is not a valid URL."
    referers = {
        path: _value(lines[0], "Referer")
        for source, paths in (
            (server, ("/document", "/none", "/chosen", "/origin", "/elsewhere", "/blob")),
            (other, ("/unsafe", "/strict")),
        )
        for path in paths
        for lines in [source.headers_of(path)]
    }
    assert referers == {
        "/document": url,
        "/none": None,
        "/chosen": server.url("/other?y=2"),
        "/origin": server.url("/"),
        "/elsewhere": url,
        "/blob": None,
        "/unsafe": url,
        "/strict": server.url("/"),
    }


def test_a_keepalive_fetch_outlives_its_document_and_another_does_not(session: mitto.Session, server: _Server) -> None:
    # Each request waits at the server until the page has gone to another document, then redirects. Only the
    # keepalive one is still there to follow it.
    held = threading.Event()
    server.routes["/kept"] = _Route(status=302, headers=[("Location", "/kept-after")], hold=held)
    server.routes["/dropped"] = _Route(status=302, headers=[("Location", "/dropped-after")], hold=held)
    server.text("/kept-after", "kept")
    server.text("/dropped-after", "dropped")
    # A page of no-store whose script's request gets a response of no-store is not kept in the back/forward cache, so it
    # unloads, as in Chrome.
    no_store = [("Cache-Control", "no-store")]
    url = server.page("/", "<p>first</p>", no_store)
    server.text("/not-stored", "", no_store)
    second = server.page("/second", "<p>second</p>")

    with _open_page(session, url) as page:
        page.evaluate("fetch('/kept', {keepalive: true, method: 'POST', body: 'beacon'}); fetch('/dropped'); null")
        assert server.wait_for("/kept")
        assert server.wait_for("/dropped")
        page.evaluate(
            "fetch('/not-stored').then(r => r.text()).then(() => { location.href = '/second'; return null; })"
        )
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS) == PageNavigation(url=second, status=200)
        held.set()
        assert server.wait_for("/kept-after")
        assert not server.wait_for("/dropped-after", timeout=1.0)


def test_keepalive_bodies_in_flight_stay_within_their_quota(session: mitto.Session, server: _Server) -> None:
    held = threading.Event()
    server.routes["/first"] = _Route(b"first", headers=[("Content-Type", "text/plain")], hold=held)
    server.text("/second", "second")
    server.text("/third", "third")
    url = server.page("/", "<p>quota</p>")

    with _open_page(session, url) as page:
        # 40 KiB in flight leaves room for 24 KiB more of a document's 64 KiB.
        page.evaluate(
            "window.first = fetch('/first', {keepalive: true, method: 'POST', body: 'x'.repeat(40 * 1024)}); null"
        )
        assert server.wait_for("/first")
        over = _outcome(page, "fetch('/second', {keepalive: true, method: 'POST', body: 'x'.repeat(30 * 1024)})")
        within = _outcome(page, "fetch('/third', {keepalive: true, method: 'POST', body: 'x'.repeat(20 * 1024)})")
        held.set()
        first = page.evaluate("first.then(response => response.text())", timeout=_TIMEOUT_SECONDS)

    assert (over, within, first) == ("TypeError: Failed to fetch", ["basic", 200], "first")
    assert server.headers_of("/second") == []


def _without_boundary(lines: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Return header lines with a multipart body's boundary and length left out, which each body has its own of."""
    return [
        (name, value.split(";")[0] if name.lower() == "content-type" else value)
        for name, value in lines
        if name.lower() != "content-length"
    ]


def test_a_beacon_goes_as_a_callers_beacon_of_the_same_body(session: mitto.Session, server: _Server) -> None:
    for path in ("/text", "/blob", "/form", "/params", "/none"):
        server.text(path, "beacon")
    url = server.page("/", "<p>beacon</p>")

    with _open_page(session, url) as page:
        sent = page.evaluate(
            """
            const form = new FormData();
            form.append('name', 'mitto');
            [
              navigator.sendBeacon('/text', 'text body'),
              navigator.sendBeacon('/blob', new Blob(['blob body'])),
              navigator.sendBeacon('/form', form),
              navigator.sendBeacon('/params', new URLSearchParams('a=1')),
              navigator.sendBeacon('/none'),
            ]
            """
        )
        for path in ("/text", "/blob", "/form", "/params", "/none"):
            assert server.wait_for(path)

    assert sent == [True, True, True, True, True]
    session.post(server.url("/text"), kind="beacon", initiator=url, data="text body")
    session.post(server.url("/blob"), kind="beacon", initiator=url, data=b"blob body")
    session.post(server.url("/form"), kind="beacon", initiator=url, data={"name": "mitto"}, files={})
    session.post(server.url("/none"), kind="beacon", initiator=url, data=b"")
    for path in ("/text", "/blob", "/none"):
        page_lines, caller_lines = server.headers_of(path)
        assert page_lines == caller_lines, path
    page_lines, caller_lines = server.headers_of("/form")
    assert _without_boundary(page_lines) == _without_boundary(caller_lines)
    (params,) = server.headers_of("/params")
    assert _value(params, "Content-Type") == "application/x-www-form-urlencoded;charset=UTF-8"
    assert _value(server.headers_of("/none")[0], "Content-Length") == "0"


def test_a_beacon_of_a_blob_of_a_type_that_is_not_safelisted_is_a_cors_request(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    # BeaconBlob puts it in mode cors, so it waits for a preflight at another origin, which allows nothing here.
    url = server.page("/", "<p>beacon</p>")
    server.text("/json", "json")

    with _open_page(session, url) as page:
        sent = page.evaluate(
            f"""
            const json = new Blob(['{{}}'], {{type: 'application/json'}});
            [navigator.sendBeacon('/json', json), navigator.sendBeacon('{other.url("/json")}', json)]
            """
        )
        assert server.wait_for("/json")
        assert other.wait_for("OPTIONS /json")

    assert sent == [True, True]
    assert _value(server.headers_of("/json")[0], "Content-Type") == "application/json"
    assert other.headers_of("/json") == []


def test_a_beacon_of_a_blob_of_a_long_safelisted_type_goes_without_a_preflight(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    # BeaconBlob checks a Blob's type as a safelisted type of any length, so Chrome 157 sent this one in mode no-cors, with
    # its type, where a script's line of more than 128 bytes would need a preflight.
    url = server.page("/", "<p>beacon</p>")
    other.text("/long", "long")
    content_type = "text/plain;a=" + "x" * 200

    with _open_page(session, url) as page:
        sent = page.evaluate(
            f"navigator.sendBeacon('{other.url('/long')}', new Blob(['x'], {{type: '{content_type}'}}))"
        )
        assert other.wait_for("/long")

    assert sent is True
    assert other.headers_of("OPTIONS /long") == []
    (lines,) = other.headers_of("/long")
    assert (_value(lines, "Content-Type"), _value(lines, "Sec-Fetch-Mode")) == (content_type, "no-cors")


def test_a_beacon_that_cannot_go_gives_false_before_its_data_is_read(session: mitto.Session, server: _Server) -> None:
    # Chrome 157 gave false for a detached window's beacon of a stream, which CanSendBeacon() refuses before the stream
    # throws, and for a Blob and a buffer past the keepalive quota.
    url = server.page("/", "<body><p>beacon</p></body>")
    server.text("/small", "small")
    server.text("/small-beacon", "small")

    with _open_page(session, url) as page:
        gave = page.evaluate(
            "(async () => {"
            " const a = document.createElement('iframe'); document.body.append(a);"
            " await new Promise((resolve) => { a.onload = resolve; a.src = '/small'; });"
            " const navigator_of_a = a.contentWindow.navigator; a.remove(); let detached;"
            " try { detached = navigator_of_a.sendBeacon('/detached', new ReadableStream()); }"
            " catch (e) { detached = String(e); }"
            " return [detached, navigator.sendBeacon('/big-blob', new Blob([new Uint8Array(70000)])),"
            " navigator.sendBeacon('/big-buffer', new Uint8Array(70000)),"
            " navigator.sendBeacon('/small-beacon', new Blob([new Uint8Array(100)]))];"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
        assert server.wait_for("/small-beacon")
    assert gave == [False, False, False, True]
    assert (server.headers_of("/detached"), server.headers_of("/big-blob"), server.headers_of("/big-buffer")) == (
        [],
        [],
        [],
    )


def test_a_beacon_stays_within_the_keepalive_quota_and_refuses_what_it_cannot_send(
    session: mitto.Session, server: _Server
) -> None:
    held = threading.Event()
    server.routes["/first"] = _Route(b"first", headers=[("Content-Type", "text/plain")], hold=held)
    server.text("/third", "third")
    url = server.page("/", "<p>quota</p>")

    with _open_page(session, url) as page:
        # 40 KiB in flight leaves room for 24 KiB more of a document's 64 KiB.
        sent = page.evaluate(
            """
            [
              navigator.sendBeacon('/first', 'x'.repeat(40 * 1024)),
              navigator.sendBeacon('/second', 'x'.repeat(30 * 1024)),
              navigator.sendBeacon('/third', 'x'.repeat(20 * 1024)),
            ]
            """
        )
        errors = page.evaluate(
            """
            [() => navigator.sendBeacon('ftp://example.test/'), () => navigator.sendBeacon('http://['),
             () => navigator.sendBeacon('/stream', new ReadableStream())].map(call => {
              try { return call(); } catch (error) { return String(error); }
            })
            """
        )
        assert server.wait_for("/third")
        held.set()

    assert sent == [True, False, True]
    assert server.headers_of("/second") == []
    assert errors == [
        "TypeError: Failed to execute 'sendBeacon' on 'Navigator': Beacons are only supported over HTTP(S).",
        "TypeError: Failed to execute 'sendBeacon' on 'Navigator': The URL argument is ill-formed or unsupported.",
        "TypeError: Failed to execute 'sendBeacon' on 'Navigator': sendBeacon cannot have a ReadableStream body.",
    ]
    assert server.headers_of("/stream") == []


def test_a_fetch_later_goes_when_its_page_closes_as_a_callers_fetch(session: mitto.Session, server: _Server) -> None:
    server.text("/later", "later")
    url = server.page("/", "<p>later</p>")

    with _open_page(session, url) as page:
        page.evaluate("window.result = fetchLater('/later'); null")
        assert not server.wait_for("/later", timeout=1.0)
        activated = page.evaluate("result.activated")
    assert server.wait_for("/later")

    assert activated is False
    session.get(server.url("/later"), kind="fetch", initiator=url)
    page_lines, caller_lines = server.headers_of("/later")
    assert page_lines == caller_lines


def test_a_fetch_later_goes_after_its_activate_after_or_with_its_document_and_never_once_aborted(
    session: mitto.Session, server: _Server
) -> None:
    for path in ("/soon", "/next", "/aborted"):
        server.text(path, path)
    url = server.page("/", "<p>later</p>")
    second = server.page("/second", "<p>second</p>")

    with _open_page(session, url) as page:
        page.evaluate(
            """
            window.soon = fetchLater('/soon', {activateAfter: 100});
            window.next = fetchLater('/next');
            const controller = new AbortController();
            window.aborted = fetchLater('/aborted', {signal: controller.signal});
            controller.abort();
            null
            """
        )
        assert server.wait_for("/soon")
        activated = page.evaluate("[soon.activated, next.activated, aborted.activated]")
        assert server.headers_of("/next") == []
        page.evaluate("location.href = '/second'; null")
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS) == PageNavigation(url=second, status=200)
        assert server.wait_for("/next")

    assert activated == [True, False, False]
    assert server.headers_of("/aborted") == []


def test_fetch_later_throws_chromes_errors_and_holds_its_quota(session: mitto.Session, server: _Server) -> None:
    url = server.page("/", "<p>later</p>")
    # The URL without its fragment, the referrer "about:client", the Content-Type line of a string and the body.
    line = len("Content-Type") + len("text/plain;charset=UTF-8")
    size = len(server.url("/x")) + len("about:client") + line + 64 * 1024

    with _open_page(session, url) as page:
        errors = page.evaluate(
            """
            [() => fetchLater('/x', {method: 'POST', body: 'x'.repeat(64 * 1024)}),
             () => fetchLater('/x', {activateAfter: -1}),
             () => fetchLater('ftp://example.test/'),
             () => fetchLater('http://example.test/'),
             () => fetchLater('/x', {signal: AbortSignal.abort()}),
             () => fetchLater('/x', {method: 'POST', body: new ReadableStream(), duplex: 'half'})
            ].map(call => { try { call(); return 'none'; } catch (error) { return String(error); } })
            """
        )
        # A request that waits takes from the quota of its origin until its signal aborts.
        quota = page.evaluate(
            """
            const controller = new AbortController();
            fetchLater('/a', {method: 'POST', body: 'x'.repeat(60 * 1024), signal: controller.signal});
            const outcome = () => {
              try { fetchLater('/b', {method: 'POST', body: 'x'.repeat(10 * 1024)}); return 'queued'; }
              catch (error) { return error.name; }
            };
            const before = outcome();
            controller.abort();
            [before, outcome()]
            """
        )

    prefix = "Failed to execute 'fetchLater' on 'Window': "
    assert errors == [
        f"QuotaExceededError: {prefix}fetchLater exceeds its quota for the origin: got {size} bytes, expected less "
        "than 65536 bytes.",
        f"RangeError: {prefix}fetchLater's activateAfter cannot be negative.",
        f"TypeError: {prefix}fetchLater is only supported over HTTP(S).",
        f"SecurityError: {prefix}fetchLater was passed an insecure URL.",
        f"AbortError: {prefix}The user aborted a fetchLater request.",
        f"TypeError: {prefix}fetchLater doesn't support body with unknown length.",
    ]
    assert quota == ["QuotaExceededError", "queued"]


# What each page and frame of the fetchLater() quota cases has: avail(url) reads what is left of the quota for a request to
# the origin of `url` from the QuotaExceededError of a 1 MiB body, fill(url, n) queues a deferred request of n bytes as
# Blink counts them, fillers are seven origins of other sites to fill, frame(src) appends an iframe and waits for its load,
# and ask(frame, script) runs a script in a frame of any origin and gives back its result, through the page's own
# postMessage(), as a page reaches a window of another origin only so until #286.
_FETCH_LATER_HELPERS = r"""
const BIG = new Uint8Array(1 << 20);
const fillers = [1, 2, 3, 4, 5, 6, 7].map((i) => `http://f${i}.localhost:${location.port}/fill`);
function avail(url) {
  try { fetchLater(url, {method: 'POST', body: BIG}); return 'no throw'; }
  catch (e) { const m = /expected less than (\d+) bytes/.exec(e.message); return m ? Number(m[1]) : e.name + ': ' + e.message; }
}
function fill(url, n) {
  const length = new URL(url, location.href).href.split('#')[0].length + 'about:client'.length;
  return fetchLater(url, {method: 'POST', body: new Uint8Array(n - length)});
}
function frame(src, attributes = {}) {
  return new Promise((resolve) => {
    const f = document.createElement('iframe');
    for (const [k, v] of Object.entries(attributes)) f.setAttribute(k, v);
    if (src !== null) f.src = src;
    f.onload = () => resolve(f);
    document.body.append(f);
  });
}
let asked = 0;
function ask(f, script) {
  const id = ++asked + ':' + Math.random();
  return new Promise((resolve) => {
    addEventListener('message', function listener(e) {
      if (e.data && e.data.id === id) { removeEventListener('message', listener); resolve(e.data.result); }
    });
    postMessage.call(f.contentWindow, {id, script}, '*');
  });
}
addEventListener('message', async (e) => {
  if (!e.data || !e.data.script) return;
  let result;
  try { result = await (0, eval)('(async () => {' + e.data.script + '})()'); } catch (error) { result = 'threw: ' + error; }
  postMessage.call(e.source, {id: e.data.id, result}, '*');
});
"""

# Each case's script at the top level of a page of its own, with B and C the origins of two other sites and F the path of
# the frame page, and what Chrome for Testing 157.0.8103.79 gave for it.
_FETCH_LATER_QUOTAS: dict[str, tuple[str, Any]] = {
    "a top-level document's quota": (
        "return avail('/x');",
        65536,
    ),
    "a top-level document's quota after seven other origins": (
        "for (const u of fillers) fill(u, 65536); return avail('/x');",
        65536,
    ),
    "a top-level document's quota beside a frame of another origin": (
        "await frame(B + F); for (const u of fillers) fill(u, 65536); return avail('/x');",
        57344,
    ),
    "a top-level document's quota beside a frame of its origin": (
        "await frame(F); for (const u of fillers) fill(u, 65536); return avail('/x');",
        65536,
    ),
    "a top-level document's quota beside a frame of another origin in a frame of its origin": (
        "const a = await frame(F); await ask(a, 'await frame(B + F); return 1;'); for (const u of "
        "fillers) fill(u, 65536); return avail('/x');",
        49152,
    ),
    "a frame of another origin": (
        "const b = await frame(B + F); return ask(b, \"return avail('/x');\");",
        8192,
    ),
    "a frame of the page's origin": (
        "const a = await frame(F); return ask(a, \"return avail('/x');\");",
        65536,
    ),
    "a frame of the page's origin shares its quota": (
        "fill('/x', 30000); const a = await frame(F); return ask(a, \"return avail('/y');\");",
        35536,
    ),
    "a frame of another origin takes what it queued": (
        "const b = await frame(B + F); return ask(b, \"fill('/x', 5000); return avail('/y');\");",
        3192,
    ),
    "a frame of another origin in one of another origin": (
        'const b = await frame(B + F); return ask(b, "const c = await frame(C + F); return ask(c, '
        '\\"return avail(\'/x\');\\");");',
        0,
    ),
    "a frame of the origin of the frame of another origin it is in": (
        'const b = await frame(B + F); return ask(b, "const d = await frame(F); return ask(d, \\"return '
        "avail('/x');\\\");\");",
        8192,
    ),
    "a frame of another origin in a frame of the page's origin": (
        'const a = await frame(F); return ask(a, "const b = await frame(B + F); return ask(b, \\"return '
        "avail('/x');\\\");\");",
        8192,
    ),
    "the sixteenth and seventeenth frames of other origins": (
        "const frames = []; for (let i = 0; i < 17; i++) frames.push(await frame(B + F)); return [await "
        "ask(frames[15], \"return avail('/x');\"), await ask(frames[16], \"return avail('/x');\")];",
        [8192, 0],
    ),
    "a frame that goes gives its quota back": (
        "const frames = []; for (let i = 0; i < 17; i++) frames.push(await frame(B + F)); "
        "frames[0].remove(); const again = await frame(B + F); return ask(again, \"return avail('/x');\");",
        8192,
    ),
    "a sandboxed frame of the page's URL": (
        "const s = await frame(F, {sandbox: 'allow-scripts'}); return ask(s, \"return avail('/x');\");",
        8192,
    ),
    "a srcdoc frame": (
        "fill('/x', 30000); const s = await frame(null, {srcdoc: '<script>' + HELPERS_SOURCE + "
        "'<\\/script>'}); return ask(s, \"return avail('/y');\");",
        35536,
    ),
    "a frame of another origin that navigates itself to the page's origin": (
        "const b = await frame(B + F); await new Promise((resolve) => { b.onload = resolve; ask(b, "
        '"location.href = A + F; return 1;"); }); return ask(b, "return avail(\'/x\');");',
        65536,
    ),
    "a frame of the page's origin that its parent navigates to another origin": (
        "const a = await frame(F); await new Promise((resolve) => { a.onload = resolve; a.src = B + F; "
        "}); return ask(a, \"return avail('/x');\");",
        8192,
    ),
}


def _fetch_later_page(server: _Server) -> str:
    """Serve the page and the frame page of the fetchLater() quota cases, with the helpers, and return the page's URL."""
    port = server.origin.rsplit(":", 1)[1]
    origins = f"const A = 'http://127.0.0.1:{port}', B = 'http://localhost:{port}', C = 'http://c.localhost:{port}', F = '/f';"
    helpers = origins + _FETCH_LATER_HELPERS
    for path in ("/x", "/y", "/fill"):
        server.text(path, "x", [("Access-Control-Allow-Origin", "*")])
    server.page("/f", f"<body><script>{helpers}</script>")
    return server.page("/", f"<body><script>const HELPERS_SOURCE = {json.dumps(helpers)}; {helpers}</script>")


def test_fetch_later_takes_the_quota_of_its_frames_deferred_fetch_control_document(
    session: mitto.Session, server: _Server
) -> None:
    # A frame of the page's origin shares the top-level document's quota, one of another site has the 8 KiB that its
    # element reserved of the top-level document's, as at most 16 elements do, and one under that has none.
    url = _fetch_later_page(server)
    gave: dict[str, Any] = {}
    for name, (script, _) in _FETCH_LATER_QUOTAS.items():
        with _open_page(session, url) as page:
            gave[name] = page.evaluate(f"(async () => {{ {script} }})()", timeout=_TIMEOUT_SECONDS)
    assert gave == {name: expected for name, (_, expected) in _FETCH_LATER_QUOTAS.items()}


def test_a_frames_deferred_requests_go_as_its_document_does(session: mitto.Session, server: _Server) -> None:
    # As Chrome 157 sends them: those of a frame of the page's origin and of another site as the frame goes, and those of
    # a document that a navigation of its frame leaves.
    url = _fetch_later_page(server)
    with _open_page(session, url) as page:
        page.evaluate(
            "(async () => {"
            " const a = await frame(F); await ask(a, \"fetchLater('/log/gone-a', {activateAfter: 600000}); return 1;\");"
            " const b = await frame(B + F); await ask(b, \"fetchLater('/log/gone-b', {activateAfter: 600000}); return 1;\");"
            " const n = await frame(B + F);"
            " await ask(n, \"fetchLater('/log/navigated', {activateAfter: 600000}); return 1;\");"
            " a.remove(); b.remove();"
            " await new Promise((resolve) => { n.onload = resolve; n.src = C + F; });"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
        assert server.wait_for("/log/gone-a")
        assert server.wait_for("/log/gone-b")
        assert server.wait_for("/log/navigated")


def test_a_frames_fetch_later_results_say_activated_as_the_frame_goes(session: mitto.Session, server: _Server) -> None:
    # FetchLaterManager::ContextDestroyed() sends each deferred request as the frame goes, before any script runs again, so
    # Chrome 157 said both had been activated right after the removal, the one whose signal then aborted too.
    url = _fetch_later_page(server)
    with _open_page(session, url) as page:
        gave = page.evaluate(
            "(async () => {"
            " const a = await frame(F); const controller = new AbortController();"
            " const kept = a.contentWindow.fetchLater('/log/kept', {activateAfter: 600000});"
            " const aborted = a.contentWindow.fetchLater('/log/aborted', {activateAfter: 600000, signal: controller.signal});"
            " const before = kept.activated; a.remove(); controller.abort();"
            " return [before, kept.activated, aborted.activated];"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
        assert server.wait_for("/log/kept")
        assert server.wait_for("/log/aborted")
    assert gave == [False, True, True]


def test_fetch_later_counts_a_string_body_by_its_length(session: mitto.Session, server: _Server) -> None:
    # Blink's ExtractBody() records a string's length, which FetchLaterUtil counts, though the body goes as UTF-8, so
    # Chrome 157 took 30,000 CJK characters within the 64 KiB of an origin, and counted 70,000 as 70,048 bytes with the
    # referrer about:client and the Content-Type line.
    url = server.page("/", "<p>later</p>")
    with _open_page(session, url) as page:
        gave = page.evaluate(
            "let fits = 'no throw'; try { fetchLater('/x', {method: 'POST', body: '\u4e2d'.repeat(30000)}); }"
            " catch (e) { fits = String(e); }"
            " let got = null; try { fetchLater('/x', {method: 'POST', body: '\u4e2d'.repeat(70000)}); } catch (e) {"
            " const m = /got (\\d+) bytes/.exec(e.message);"
            " got = m ? Number(m[1]) - new URL('/x', location.href).href.length : String(e); }"
            " [fits, got]",
            timeout=_TIMEOUT_SECONDS,
        )
    assert gave == ["no throw", 70048]


def test_a_borrowed_send_beacon_and_fetch_later_go_as_their_receivers_window(
    session: mitto.Session, server: _Server
) -> None:
    # Blink's bindings give a non-static operation its receiver's ScriptState, so Chrome 157 resolved each URL against the
    # frame's, sent each with the frame's URL as Referer, the deferred request as the frame went, and gave false for a
    # detached frame's navigator.
    url = _fetch_later_page(server)
    server.page("/sub/f", f"<body><script>{_FETCH_LATER_HELPERS}</script>")
    for path in (
        "/sub/log/beacon",
        "/sub/log/later",
        "/sub/log/detached",
        "/log/beacon",
        "/log/later",
        "/log/detached",
    ):
        server.text(path, "x")
    with _open_page(session, url) as page:
        gave = page.evaluate(
            "(async () => {"
            " const a = await frame('/sub/f');"
            " const sent = Navigator.prototype.sendBeacon.call(a.contentWindow.navigator, 'log/beacon');"
            " const result = fetchLater.call(a.contentWindow, 'log/later', {activateAfter: 600000});"
            " const d = await frame('/sub/f'); const navigator_of_d = d.contentWindow.navigator; d.remove();"
            " const detached = Navigator.prototype.sendBeacon.call(navigator_of_d, 'log/detached');"
            " a.remove(); return [sent, result.activated, detached];"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
        assert server.wait_for("/sub/log/beacon")
        assert server.wait_for("/sub/log/later")
    assert gave == [True, True, False]
    frame_url = server.url("/sub/f")
    assert _value(server.headers_of("/sub/log/beacon")[0], "Referer") == frame_url
    assert _value(server.headers_of("/sub/log/later")[0], "Referer") == frame_url
    assert [server.headers_of(path) for path in ("/log/beacon", "/log/later", "/sub/log/detached")] == [[], [], []]


def test_fetch_later_counts_a_cloned_requests_body(session: mitto.Session, server: _Server) -> None:
    # A clone keeps its body's length, as Blink's FetchRequestData::Clone() does, so Chrome 157 refused this one.
    url = server.page("/", "<p>later</p>")
    with _open_page(session, url) as page:
        gave = page.evaluate(
            "const request = new Request('/x', {method: 'POST', body: 'a'.repeat(70000)});"
            " let name = 'no throw'; try { fetchLater(request.clone()); } catch (e) { name = e.name; } name",
            timeout=_TIMEOUT_SECONDS,
        )
    assert gave == "QuotaExceededError"


def test_a_detached_frames_fetch_later_throws_before_it_takes_its_input(
    session: mitto.Session, server: _Server
) -> None:
    # GlobalFetch's ScopedFetcher refuses a context that has gone before it makes the Request, so the body stays unused.
    url = _fetch_later_page(server)
    with _open_page(session, url) as page:
        gave = page.evaluate(
            "(async () => {"
            " const a = await frame(F); const kept = a.contentWindow.fetchLater; a.remove();"
            " const request = new Request('/x', {method: 'POST', body: 'a'}); let error = 'no throw';"
            " try { kept(request); } catch (e) { error = String(e); }"
            " return [error, request.bodyUsed];"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
    assert gave == ["TypeError: Failed to execute 'fetchLater' on 'Window': The global scope is shutting down.", False]


def test_a_deferred_request_past_the_keepalive_requests_in_flight_never_goes(
    session: mitto.Session, server: _Server
) -> None:
    # The network service counts a deferred request as the browser starts it, past 256 keepalive requests of the page in
    # flight, so Chrome 157 never sent the one that went then, but sent the next once they had ended.
    url = server.page("/", "<p>held</p>")
    held = threading.Event()
    for index in range(257):
        server.routes[f"/hold{index}"] = _Route(b"held", headers=[("Content-Type", "text/plain")], hold=held)
    server.text("/over", "over")
    server.text("/after", "after")

    with _open_page(session, url) as page:
        one_more = page.evaluate(
            "(async () => {"
            " window.held = []; for (let i = 0; i < 256; i++) held.push(fetch('/hold' + i, {keepalive: true}).catch(() => 0));"
            " const one_more = await fetch('/hold256', {keepalive: true}).then(() => 'ok', (e) => String(e));"
            " fetchLater('/over', {activateAfter: 0}); await new Promise((r) => setTimeout(r, 500)); return one_more;"
            " })()",
            timeout=_TIMEOUT_SECONDS,
        )
        held.set()
        page.evaluate("Promise.all(held).then(() => 0)", timeout=_TIMEOUT_SECONDS)
        page.evaluate("fetchLater('/after', {activateAfter: 0}); 0", timeout=_TIMEOUT_SECONDS)
        assert server.wait_for("/after")
    assert one_more == "TypeError: Failed to fetch"
    assert server.headers_of("/over") == []


def test_a_scripts_crossorigin_attribute_makes_its_request_a_cors_one(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    script = [("Content-Type", "text/javascript")]
    other.routes["/plain.js"] = _Route(b"window.ran.push('plain')", headers=script)
    other.routes["/anonymous.js"] = _Route(
        b"window.ran.push('anonymous')", headers=[*script, ("Access-Control-Allow-Origin", "*")]
    )
    other.routes["/credentials.js"] = _Route(
        b"window.ran.push('credentials')",
        headers=[
            *script,
            ("Access-Control-Allow-Origin", server.origin),
            ("Access-Control-Allow-Credentials", "true"),
        ],
    )
    other.routes["/closed.js"] = _Route(b"window.ran.push('closed')", headers=script)
    other.routes["/private.js"] = _Route(b"window.ran.push('private')", headers=script)
    url = server.page(
        "/",
        "<script>window.ran = []</script>"
        f"<script src='{other.url('/plain.js')}'></script>"
        f"<script src='{other.url('/anonymous.js')}' crossorigin></script>"
        f"<script src='{other.url('/credentials.js')}' crossorigin='USE-CREDENTIALS'></script>"
        f"<script src='{other.url('/closed.js')}' crossorigin='anonymous'></script>"
        f"<script src='{other.url('/private.js')}' referrerpolicy='no-referrer'></script>",
        [("Set-Cookie", "who=mitto")],
    )

    with _open_page(session, url) as page:
        ran = page.evaluate("window.ran")

    # One that fails the CORS check does not run.
    assert ran == ["plain", "anonymous", "credentials", "private"]
    lines = {
        path: other.headers_of(path)[0] for path in ("/plain.js", "/anonymous.js", "/credentials.js", "/private.js")
    }
    assert {path: _value(found, "Sec-Fetch-Mode") for path, found in lines.items()} == {
        "/plain.js": "no-cors",
        "/anonymous.js": "cors",
        "/credentials.js": "cors",
        "/private.js": "no-cors",
    }
    assert {path: _value(found, "Cookie") for path, found in lines.items()} == {
        "/plain.js": "who=mitto",
        "/anonymous.js": None,
        "/credentials.js": "who=mitto",
        "/private.js": "who=mitto",
    }
    assert _value(lines["/anonymous.js"], "Origin") == server.origin
    assert _value(lines["/plain.js"], "Origin") is None
    assert _value(lines["/plain.js"], "Referer") == server.url("/")
    assert _value(lines["/private.js"], "Referer") is None
    # A script in cors mode goes as a caller's script request in that mode does.
    session.get(other.url("/anonymous.js"), kind="script", initiator=url, mode="cors", credentials="same-origin")
    page_lines, caller_lines = other.headers_of("/anonymous.js")
    assert page_lines == caller_lines


def test_the_cache_api_keeps_an_opaque_response_opaque(session: mitto.Session, server: _Server, other: _Server) -> None:
    url = server.page("/", "<p>cache</p>")
    other.text("/closed", "closed")

    with _open_page(session, url) as page:
        kept = page.evaluate(
            "(async () => { const cache = await caches.open('opaque');"
            f" await cache.put('/kept', await fetch('{other.url('/closed')}', {{mode: 'no-cors'}}));"
            " const response = await cache.match('/kept');"
            " return [response.type, response.status, response.url, await response.text()]; })()",
            timeout=_TIMEOUT_SECONDS,
        )

    assert kept == ["opaque", 0, "", ""]
