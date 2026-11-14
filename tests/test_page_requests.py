"""Tests of what a Page's requests say of themselves: fetch()'s credentials, mode, referrer, referrer policy and keepalive,
and a classic script's crossorigin and referrerpolicy, through Session._open_page() and AsyncSession._open_page(), which
stay private until M10.

Two local servers stand for the page's origin and another, on the same host, so the Session's cookies go to both and a
request to the other is same-site but cross-origin. Where a caller can ask for the same request, the lines that the Page
sent are checked against the caller's, as the Session's Request Kinds make both. Each test needs a native library with
the Page program beside it.
"""

import http.server
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

    with _open_page(session, url) as page:
        outcomes = [
            _outcome(page, f"fetch('{other.url('/any')}')"),
            _outcome(page, f"fetch('{other.url('/named')}', {{credentials: 'include'}})"),
            # A response that allows any origin does not pass for a request with credentials.
            _outcome(page, f"fetch('{other.url('/any')}', {{credentials: 'include'}})"),
            _outcome(page, "fetch('/omitted', {credentials: 'omit'})"),
        ]

    assert outcomes == [["cors", 200], ["cors", 200], "TypeError: Failed to fetch", ["basic", 200]]
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
    for path in ("/document", "/none", "/chosen", "/origin", "/elsewhere"):
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
            _outcome(page, f"fetch('{other.url('/unsafe')}', {{referrerPolicy: 'unsafe-url'}})"),
            _outcome(page, f"fetch('{other.url('/strict')}')"),
        ]
        invalid = page.evaluate("try { new Request('/x', {referrer: 'http://['}); 'made' } catch (e) { String(e) }")

    assert outcomes[:5] == [["basic", 200]] * 5
    assert outcomes[5:] == [["cors", 200]] * 2
    assert invalid == "TypeError: Failed to construct 'Request': Referrer 'http://[' is not a valid URL."
    referers = {
        path: _value(lines[0], "Referer")
        for source, paths in (
            (server, ("/document", "/none", "/chosen", "/origin", "/elsewhere")),
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
    url = server.page("/", "<p>first</p>")
    second = server.page("/second", "<p>second</p>")

    with _open_page(session, url) as page:
        page.evaluate("fetch('/kept', {keepalive: true, method: 'POST', body: 'beacon'}); fetch('/dropped'); null")
        assert server.wait_for("/kept")
        assert server.wait_for("/dropped")
        page.evaluate("location.href = '/second'; null")
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
