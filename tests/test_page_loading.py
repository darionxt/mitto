"""Tests of a Page that loads its document through its Session: Session._open_page() and AsyncSession._open_page(), which
stay private until M10.

A local server serves each page and its scripts, and records what each request carried. Each test goes through both APIs,
and needs a native library with the Page program beside it, as CI's native job installs it on every Host.
"""

import contextlib
import http.server
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._errors import PageTimeoutError
from mitto._page import Page
from tests.local_server import QuietServer, refused_port

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_HTML = "text/html; charset=utf-8"
_JAVASCRIPT = "text/javascript"


@dataclass
class _Route:
    """What the server answers for one path: a status, header lines and a body, or a function that writes the body.

    With `hold`, the server answers only once it is set.
    """

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    write: Callable[[http.server.BaseHTTPRequestHandler], None] | None = None
    hold: threading.Event | None = None


class _PageServer:
    """A local server for pages and their scripts, which records the header lines of each request it gets, in order.

    An OPTIONS request is recorded and routed as "OPTIONS PATH", and answered as GET at PATH where no route has that name.
    """

    def __init__(self) -> None:
        self.routes: dict[str, _Route] = {}
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.bodies: dict[str, list[bytes]] = {}
        self.requested = threading.Condition()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self) -> None:
                size = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(size)
                with server.requested:
                    server.bodies.setdefault(self.path, []).append(body)
                self.do_GET()

            def do_GET(self) -> None:
                path = f"OPTIONS {self.path}" if self.command == "OPTIONS" else self.path
                with server.requested:
                    server.requests.append((path, list(self.headers.items())))
                    server.requested.notify_all()
                not_here = _Route(b"not here", 404, [("Content-Type", "text/plain")])
                route = server.routes.get(path, server.routes.get(self.path, not_here))
                if route.hold is not None:
                    route.hold.wait(_TIMEOUT_SECONDS)
                self.send_response(route.status)
                for name, value in route.headers:
                    self.send_header(name, value)
                if route.write is not None:
                    self.send_header("Connection", "close")
                    self.end_headers()
                    route.write(self)
                    return
                self.send_header("Content-Length", str(len(route.body)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(route.body)

            def do_HEAD(self) -> None:
                self.do_GET()

            def do_OPTIONS(self) -> None:
                self.do_GET()

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def page(self, path: str, markup: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = _Route(markup.encode(), headers=[("Content-Type", _HTML), *(headers or [])])
        return self.url(path)

    def script(self, path: str, source: str, status: int = 200) -> None:
        self.routes[path] = _Route(source.encode(), status, [("Content-Type", _JAVASCRIPT)])

    def json(self, path: str, text: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = _Route(text.encode(), headers=[("Content-Type", "application/json"), *(headers or [])])
        return self.url(path)

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        """Return the header lines of each request for `path`, in the order they came."""
        with self.requested:
            return [headers for requested, headers in self.requests if requested == path]

    def wait_for(self, path: str, timeout: float = _TIMEOUT_SECONDS) -> bool:
        """Return whether a request for `path` has come, waiting up to `timeout` for one."""
        with self.requested:
            return self.requested.wait_for(lambda: any(requested == path for requested, _ in self.requests), timeout)

    def __enter__(self) -> "_PageServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_PageServer]:
    with _PageServer() as page_server:
        yield page_server


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session through each API in turn, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str, timeout: float | None = _TIMEOUT_SECONDS) -> Page:
    return session._open_page(url, timeout=timeout)  # pyright: ignore[reportPrivateUsage]


def _refused_url() -> str:
    """Return a URL of a local port that nothing listens on."""
    return f"http://127.0.0.1:{refused_port()}/"


def test_a_page_loads_its_document_and_runs_its_classic_scripts_in_document_order(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script(
        "/app.js", "seen.push('external, #after ' + (document.getElementById('after') ? 'there' : 'not yet'));"
    )
    url = server.page(
        "/",
        "<!doctype html><title>Loaded</title><p id=before>before</p>"
        "<script>window.seen = ['inline, ' + document.readyState + ', #before ' + document.getElementById('before').id];"
        "</script><script src=/app.js></script><p id=after>after</p>"
        "<script>seen.push('last'); document.getElementById('after').textContent = 'changed';</script>",
    )

    with _open_page(session, url) as page:
        assert page.url == url
        assert page.evaluate("[document.URL, document.readyState, document.title, seen]") == [
            url,
            "complete",
            "Loaded",
            ["inline, loading, #before before", "external, #after not yet", "last"],
        ]
        assert page.evaluate("document.body.lastElementChild.previousElementSibling.textContent") == "changed"
        assert page.uncaught_errors == []


def test_a_page_asks_for_its_document_and_scripts_as_its_session_asks_for_those_kinds(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/app.js", "window.ran = true;")
    url = server.page("/", "<script src=/app.js></script>")

    with _open_page(session, url) as page:
        assert page.evaluate("ran") is True
    session.get(url)
    session.get(server.url("/app.js"), kind="script", initiator=url)

    # The same header lines in the same order: a navigation that the browser started, and a script of that document.
    navigations = server.headers_of("/")
    scripts = server.headers_of("/app.js")
    assert [len(navigations), len(scripts)] == [2, 2]
    assert navigations[0] == navigations[1]
    assert scripts[0] == scripts[1]
    assert ("Sec-Fetch-Dest", "document") in navigations[0]
    assert ("Sec-Fetch-Dest", "script") in scripts[0]
    assert ("Referer", url) in scripts[0]


def test_a_page_follows_redirects_and_its_document_has_the_url_it_ends_at(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/app.js", "window.ran = true;")
    final_url = server.page("/document", "<script src=/app.js></script>")
    server.routes["/start"] = _Route(status=302, headers=[("Location", "/document")])

    with _open_page(session, server.url("/start")) as page:
        assert page.url == final_url
        assert page.evaluate("[document.URL, ran]") == [final_url, True]
    # The document's URL is its scripts' Initiator.
    assert ("Referer", final_url) in server.headers_of("/app.js")[0]


def test_document_cookie_reads_and_writes_the_sessions_cookies_as_a_pages_script_does(
    session: mitto.Session, server: _PageServer
) -> None:
    url = server.page(
        "/",
        "<script>window.first = document.cookie; document.cookie = 'script=3';"
        "document.cookie = 'sneaky=4; HttpOnly'; window.second = document.cookie;</script>",
        [("Set-Cookie", "server=1"), ("Set-Cookie", "secret=2; HttpOnly")],
    )
    server.routes["/next"] = _Route(b"ok", headers=[("Content-Type", "text/plain")])

    with _open_page(session, url) as page:
        # No HttpOnly cookie, and none that a script sets.
        assert page.evaluate("[first, second, document.cookie]") == [
            "server=1",
            "server=1; script=3",
            "server=1; script=3",
        ]
    session.get(server.url("/next"))

    assert ("Cookie", "server=1; secret=2; script=3") in server.headers_of("/next")[0]
    sources = {cookie.name: cookie.source_type for cookie in session.cookies}
    assert sources == {"server": "http", "secret": "http", "script": "script"}


def test_a_document_goes_through_loading_interactive_and_complete_with_its_events(
    session: mitto.Session, server: _PageServer
) -> None:
    url = server.page(
        "/",
        "<script>window.seen = [document.readyState];"
        "document.addEventListener('readystatechange', () => seen.push('readystatechange ' + document.readyState));"
        "document.addEventListener('DOMContentLoaded', e => seen.push('DOMContentLoaded ' + document.readyState"
        " + ' ' + e.bubbles + ' ' + e.isTrusted));"
        "window.addEventListener('load', e => seen.push('load ' + document.readyState + ' '"
        " + (e.target === document) + ' ' + (e.currentTarget === window)));"
        "</script><p>body</p>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("seen") == [
            "loading",
            "readystatechange interactive",
            "DOMContentLoaded interactive true true",
            "readystatechange complete",
            "load complete true true",
        ]


def test_a_script_that_cannot_be_fetched_does_not_run_and_the_parser_goes_on(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/missing.js", "window.missing = true;", status=404)
    server.script("/fine.js", "seen.push('fine ran');")
    url = server.page(
        "/",
        "<script>window.seen = [];</script><script src=/missing.js></script>"
        f"<script src='{_refused_url()}refused.js'></script><script src=''></script><script src=/fine.js></script>"
        "<p id=end>end</p>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("[typeof missing, seen, document.getElementById('end').id]") == [
            "undefined",
            ["fine ran"],
            "end",
        ]
        assert page.uncaught_errors == []


def test_a_script_fires_load_once_it_has_run_and_error_when_it_cannot_run(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/fine.js", "seen.push('ran');")
    server.script("/missing.js", "seen.push('never');", status=404)
    # Deferred scripts and one with no URL are in the tree before their events, so a later script can listen for them.
    url = server.page(
        "/",
        "<script>window.seen = [];</script>"
        "<script defer src=/fine.js></script><script defer src=/missing.js></script><script src=''></script>"
        "<script>for (const name of ['/fine.js', '/missing.js', '']) {"
        "  const script = document.querySelector(`script[src='${name}']`);"
        "  script.addEventListener('load', e => seen.push(`load '${name}' ${e.isTrusted}`));"
        "  script.addEventListener('error', e => seen.push(`error '${name}' ${e.isTrusted}`));"
        "}</script>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("seen") == [
            "error '' true",
            "ran",
            "load '/fine.js' true",
            "error '/missing.js' true",
        ]


def test_a_script_whose_mime_type_cannot_run_fires_error_as_in_chrome(
    session: mitto.Session, server: _PageServer
) -> None:
    def script(path: str, content_type: str, nosniff: bool) -> None:
        headers = [("Content-Type", content_type), *([("X-Content-Type-Options", "nosniff")] if nosniff else [])]
        server.routes[path] = _Route(f"seen.push('{path} ran');".encode(), headers=headers)

    script("/plain.js", "text/plain", nosniff=True)
    script("/image.js", "image/png", nosniff=False)
    script("/javascript.js", "Text/JavaScript; charset=utf-8", nosniff=True)
    script("/lax.js", "text/plain", nosniff=False)
    url = server.page(
        "/",
        "<script>window.seen = [];</script>"
        "<script defer src=/plain.js></script><script defer src=/image.js></script>"
        "<script defer src=/javascript.js></script><script defer src=/lax.js></script>"
        "<script>for (const name of ['/plain.js', '/image.js']) {"
        "  document.querySelector(`script[src='${name}']`).addEventListener('error', () => seen.push(name + ' error'));"
        "}</script>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("seen") == ["/plain.js error", "/image.js error", "/javascript.js ran", "/lax.js ran"]
        # The console says why as each response ends, as Blink's ClassicPendingScript::NotifyFinished() does, so in the
        # order the responses came.
        assert sorted(message.text for message in page.console_messages if message.level == "error") == [
            f"Refused to execute script from '{server.url('/image.js')}' because its MIME type ('image/png') is not"
            " executable.",
            f"Refused to execute script from '{server.url('/plain.js')}' because its MIME type ('text/plain') is not"
            " executable, and strict MIME type checking is enabled.",
        ]


def test_a_deferred_script_runs_after_parsing_and_before_domcontentloaded(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/deferred.js", "seen.push('deferred sees #end: ' + !!document.getElementById('end'));")
    url = server.page(
        "/",
        "<script>window.seen = []; document.addEventListener('DOMContentLoaded', () => seen.push('dcl'));</script>"
        "<script defer src=/deferred.js></script><script>seen.push('inline');</script><p id=end></p>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("seen") == ["inline", "deferred sees #end: true", "dcl"]


def test_scripts_that_are_not_classic_ones_do_not_run(session: mitto.Session, server: _PageServer) -> None:
    url = server.page(
        "/",
        "<script>window.seen = [];</script>"
        "<script type=module>seen.push('module');</script>"
        "<script nomodule>seen.push('nomodule');</script>"
        '<script type=application/json>{"data": 1}</script>'
        "<script type=' TEXT/JavaScript '>seen.push('javascript type');</script>"
        "<script language=javascript>seen.push('javascript language');</script>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("seen") == ["javascript type", "javascript language"]


def test_an_error_that_a_script_leaves_uncaught_is_reported_and_the_parser_goes_on(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/broken.js", "throw new RangeError('from a file');")
    url = server.page(
        "/",
        "<script>throw new TypeError('inline');</script><script src=/broken.js></script>"
        "<script>window.reached = true;</script>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("reached") is True
        assert page.uncaught_errors == ["Uncaught TypeError: inline", "Uncaught RangeError: from a file"]


def test_the_parser_takes_the_document_as_it_arrives(session: mitto.Session, server: _PageServer) -> None:
    # The rest of the document waits until the page has asked for its first script, which it can only do once it has
    # parsed what came before.
    server.script("/gate.js", "window.gate = 'passed';")
    asked = threading.Event()

    def write(handler: http.server.BaseHTTPRequestHandler) -> None:
        handler.wfile.write(b"<!doctype html><script src=/gate.js></script>")
        handler.wfile.flush()
        if server.wait_for("/gate.js"):
            asked.set()
        handler.wfile.write(b"<p id=rest>rest</p>")

    server.routes["/"] = _Route(headers=[("Content-Type", _HTML)], write=write)

    with _open_page(session, server.url("/")) as page:
        assert asked.is_set()
        assert page.evaluate("[gate, document.getElementById('rest').textContent]") == ["passed", "rest"]


def test_a_large_document_streams_through_in_full(session: mitto.Session, server: _PageServer) -> None:
    # Many times what the Page lets the Session send before it takes the bytes, with text that UTF-8 writes in several
    # bytes, so that sequences cross the pieces it arrives in.
    text = "naïve café ☕ 𝄞 " * 40_000
    url = server.page("/", f"<!doctype html><p id=text>{text}</p>")

    with _open_page(session, url) as page:
        # JavaScript counts UTF-16 code units, two for 𝄞.
        assert page.evaluate("document.getElementById('text').textContent.length") == len(text.encode("utf-16-le")) // 2
        assert page.evaluate("document.getElementById('text').textContent.includes('\\ufffd')") is False
        assert page.evaluate("document.getElementById('text').textContent.slice(-17)") == text[-16:]


def test_a_navigation_that_fails_raises_what_a_request_for_the_url_raises(session: mitto.Session) -> None:
    url = _refused_url()
    with pytest.raises(mitto.RequestError) as from_session:
        session.get(url)

    with pytest.raises(mitto.RequestError) as from_page:
        _open_page(session, url)

    assert type(from_page.value) is type(from_session.value)
    assert from_page.value.net_error == from_session.value.net_error


def test_a_document_that_does_not_load_in_time_raises_a_timeout(session: mitto.Session, server: _PageServer) -> None:
    release = threading.Event()

    def write(handler: http.server.BaseHTTPRequestHandler) -> None:
        handler.wfile.write(b"<!doctype html><p>never ends")
        handler.wfile.flush()
        release.wait(_TIMEOUT_SECONDS)

    server.routes["/"] = _Route(headers=[("Content-Type", _HTML)], write=write)
    try:
        with pytest.raises(PageTimeoutError):
            _open_page(session, server.url("/"), timeout=0.5)
    finally:
        release.set()


def test_a_page_at_about_blank_has_no_cookies_to_read(session: mitto.Session) -> None:
    with _open_page(session, "about:blank") as page:
        assert (
            page.evaluate("try { document.cookie; 'read' } catch (e) { e.name + ': ' + e.message }")
            == "SecurityError: Failed to read the 'cookie' property from 'Document': Access is denied for this document."
        )


def test_a_page_waits_on_timers_then_fetches_json_with_the_sessions_cookies_into_its_document(
    session: mitto.Session, server: _PageServer
) -> None:
    # The Runtime skeleton's whole path: a script waits on two timers, fetches JSON from where its document came from,
    # with the Session's cookies, and writes it into the document.
    server.routes["/login"] = _Route(b"ok", headers=[("Content-Type", "text/plain"), ("Set-Cookie", "who=mitto")])
    data_url = server.json("/data.json", '{"greeting": "hello", "items": [1, 2]}')
    url = server.page(
        "/",
        "<p id=out>waiting</p><script>"
        "window.done = new Promise(resolve => setTimeout(resolve, 20))"
        ".then(() => new Promise(resolve => setTimeout(resolve, 10)))"
        ".then(() => fetch('/data.json'))"
        ".then(response => response.json())"
        ".then(data => { document.getElementById('out').textContent = data.greeting + ' ' + data.items.join('+'); "
        "return document.body.innerHTML; });"
        "</script>",
    )
    session.get(server.url("/login"))

    with _open_page(session, url) as page:
        assert page.evaluate("done", timeout=_TIMEOUT_SECONDS) == '<p id="out">hello 1+2</p>' + page.evaluate(
            "document.querySelector('script').outerHTML"
        )
    session.get(data_url, kind="fetch", initiator=url)

    # What the server saw of the page's fetch is what the Session sends for a fetch of that page's, line for line.
    fetches = server.headers_of("/data.json")
    assert len(fetches) == 2
    assert fetches[0] == fetches[1]
    assert ("Cookie", "who=mitto") in fetches[0]
    assert ("Sec-Fetch-Mode", "cors") in fetches[0]
    assert ("Sec-Fetch-Dest", "empty") in fetches[0]
    assert ("Referer", url) in fetches[0]


def test_a_fetch_with_a_body_and_header_lines_sends_what_a_sessions_fetch_with_them_sends(
    session: mitto.Session, server: _PageServer
) -> None:
    server.routes["/echo"] = _Route(b"posted", headers=[("Content-Type", "text/plain")])
    url = server.page("/", "<p>post</p>")

    with _open_page(session, url) as page:
        script = (
            "fetch('/echo', {method: 'post', body: 'hi there', headers: {'X-Test': '1', 'Cookie': 'forged=1'}})"
            ".then(response => response.text())"
        )
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == "posted"
    session.post(server.url("/echo"), data="hi there", kind="fetch", initiator=url, headers={"X-Test": "1"})

    # The method written in upper case, the forbidden Cookie line dropped, and the body's Content-Type where the
    # Persona puts it.
    posts = server.headers_of("/echo")
    assert len(posts) == 2
    assert posts[0] == posts[1]
    assert ("Content-Type", "text/plain;charset=UTF-8") in posts[0]
    assert not any(name == "Cookie" for name, _ in posts[0])
    assert server.bodies["/echo"] == [b"hi there", b"hi there"]


def test_a_response_gives_its_status_headers_and_body_as_text_json_and_bytes(
    session: mitto.Session, server: _PageServer
) -> None:
    server.routes["/item"] = _Route(
        b'\xef\xbb\xbf{"a": 1}',
        201,
        [("Content-Type", "application/json"), ("X-Extra", "yes"), ("Set-Cookie", "kept=1")],
    )
    url = server.page("/", "<p>response</p>")

    with _open_page(session, url) as page:
        script = (
            "Promise.all([fetch('/item'), fetch('/item'), fetch('/item')]).then(async ([a, b, c]) => {"
            "  const first = [a.status, a.ok, a.statusText, a.type, a.url === location.href.replace(/\\/$/, '') + '/item',"
            "                 a.redirected, a.headers.get('x-extra'), a.headers.get('set-cookie'), a.bodyUsed];"
            "  const json = await a.json();"
            "  return [first, json, a.bodyUsed, await b.text(), Array.from(new Uint8Array(await c.arrayBuffer())).length];"
            "})"
        )
        status_text = "Created"
        assert page.evaluate("1") == 1
        result = page.evaluate(script.replace("location.href", f"'{url}'"), timeout=_TIMEOUT_SECONDS)
    # The byte order mark goes for text and JSON, and stays in the bytes; Set-Cookie never reaches a script.
    assert result == [
        [201, True, status_text, "basic", True, False, "yes", None, False],
        {"a": 1},
        True,
        '{"a": 1}',
        11,
    ]
    assert {cookie.name for cookie in session.cookies} == {"kept"}


def test_a_body_that_is_read_twice_or_is_not_json_rejects_as_in_chrome(
    session: mitto.Session, server: _PageServer
) -> None:
    server.json("/bad", "not json")
    url = server.page("/", "<p>bad</p>")

    with _open_page(session, url) as page:
        with pytest.raises(mitto.MittoError, match=r"^Uncaught \(in promise\) SyntaxError: "):
            page.evaluate("fetch('/bad').then(response => response.json())", timeout=_TIMEOUT_SECONDS)
        message = page.evaluate(
            "fetch('/bad').then(async response => { await response.text();"
            " try { await response.text(); } catch (error) { return String(error); } })",
            timeout=_TIMEOUT_SECONDS,
        )
    assert message == "TypeError: Failed to execute 'text' on 'Response': body stream already read"


def test_a_fetch_that_cannot_reach_its_server_rejects_with_failed_to_fetch(
    session: mitto.Session, server: _PageServer
) -> None:
    url = server.page("/", "<p>refused</p>")

    with (
        _open_page(session, url) as page,
        pytest.raises(mitto.MittoError, match=r"^Uncaught \(in promise\) TypeError: Failed to fetch$"),
    ):
        page.evaluate(f"fetch('{_refused_url()}')", timeout=_TIMEOUT_SECONDS)


# What Chrome 157.0.8103.51 gave when a signal aborted a fetch() at each point, from a page of a local server whose
# /silent never answers, /drip sends a part of its body and then waits, and /small sends its whole body at once.
_FETCH_ABORTS = {
    "before the response": (
        "const c = new AbortController(); const p = fetch('/silent', {signal: c.signal});"
        " setTimeout(() => c.abort(), 200);"
        " try { await p; return 'resolved'; } catch (e) { return [e.name, e.message, e === c.signal.reason].join(); }",
        "AbortError,signal is aborted without reason,true",
    ),
    "a timeout before the response": (
        "try { await fetch('/silent', {signal: AbortSignal.timeout(200)}); return 'resolved'; }"
        " catch (e) { return [e.name, e.message].join(); }",
        "TimeoutError,signal timed out",
    ),
    "after the response, before a read": (
        "const c = new AbortController(); const r = await fetch('/drip', {signal: c.signal}); c.abort('why');"
        " const out = [r.bodyUsed]; for (let i = 0; i < 2; i++) {"
        " try { await r.text(); out.push('read'); } catch (e) { out.push(String(e), r.bodyUsed); } }"
        " return out.join();",
        "false,why,false,why,false",
    ),
    "during a read": (
        "const c = new AbortController(); const r = await fetch('/drip', {signal: c.signal}); const t = r.text();"
        " c.abort('why'); try { await t; return 'read'; } catch (e) { return [String(e), r.bodyUsed].join(); }",
        "why,true",
    ),
    "during an arrayBuffer read, with the default reason": (
        "const c = new AbortController(); const r = await fetch('/drip', {signal: c.signal});"
        " const t = r.arrayBuffer(); c.abort(); try { await t; return 'read'; }"
        " catch (e) { return [e.name, e.message, e === c.signal.reason].join(); }",
        "AbortError,signal is aborted without reason,true",
    ),
    "after the whole body came, before a read": (
        "const c = new AbortController(); const r = await fetch('/small', {signal: c.signal});"
        " await new Promise((resolve) => setTimeout(resolve, 300)); c.abort('late');"
        " try { return await r.text(); } catch (e) { return 'rejected ' + e; }",
        "rejected late",
    ),
    "after a read": (
        "const c = new AbortController(); const r = await fetch('/small', {signal: c.signal});"
        " const text = await r.text(); c.abort('late'); return [text, r.bodyUsed].join();",
        "small,true",
    ),
    "a json read after abort": (
        "const c = new AbortController(); const r = await fetch('/small', {signal: c.signal}); c.abort('late');"
        " try { await r.json(); return 'read'; } catch (e) { return String(e); }",
        "late",
    ),
    "a HEAD request's unread body after abort": (
        "const c = new AbortController(); const r = await fetch('/small', {method: 'HEAD', signal: c.signal});"
        " c.abort('late'); try { return JSON.stringify(await r.text()); } catch (e) { return 'rejected ' + e; }",
        "rejected late",
    ),
    "a Request's signal": (
        "const c = new AbortController(); const p = fetch(new Request('/silent', {signal: c.signal}));"
        " c.abort('via request'); try { await p; return 'resolved'; } catch (e) { return String(e); }",
        "via request",
    ),
    "init's signal over the Request's": (
        "const a = new AbortController(); const b = new AbortController();"
        " const p = fetch(new Request('/silent', {signal: a.signal}), {signal: b.signal}); a.abort('a');"
        " setTimeout(() => b.abort('b'), 100); try { await p; return 'resolved'; } catch (e) { return String(e); }",
        "b",
    ),
}


def test_a_fetchs_signal_aborts_it_as_in_chrome(session: mitto.Session, server: _PageServer) -> None:
    release = threading.Event()

    def drip(handler: http.server.BaseHTTPRequestHandler) -> None:
        handler.wfile.write(b"first")
        handler.wfile.flush()
        release.wait(_TIMEOUT_SECONDS)
        # The Session may have cancelled the request.
        with contextlib.suppress(OSError):
            handler.wfile.write(b"rest")

    server.routes["/silent"] = _Route(b"never", headers=[("Content-Type", "text/plain")], hold=release)
    server.routes["/drip"] = _Route(headers=[("Content-Type", "text/plain")], write=drip)
    server.routes["/small"] = _Route(b"small", headers=[("Content-Type", "text/plain")])
    url = server.page("/", "<p>abort</p>")

    try:
        with _open_page(session, url) as page:
            outcomes = {
                name: page.evaluate(f"(async () => {{ {script} }})()", timeout=_TIMEOUT_SECONDS)
                for name, (script, _) in _FETCH_ABORTS.items()
            }
            assert page.uncaught_errors == []
    finally:
        release.set()
    assert outcomes == {name: chrome for name, (_, chrome) in _FETCH_ABORTS.items()}


def test_a_body_that_no_script_reads_holds_the_server_back_until_one_does(
    session: mitto.Session, server: _PageServer
) -> None:
    # A Page gives its Session credit for a body only as a script reads it, so a large body that nothing reads stops
    # the server, as a full Mojo data pipe does in Chrome. The system's socket buffers take some first, which on
    # Windows' loopback is more than 16 MiB, so the body is far larger, and the server writes it a piece at a time.
    piece = b"x" * (1024 * 1024)
    pieces = 64
    progress = {"written": 0}
    written = threading.Event()

    def write(handler: http.server.BaseHTTPRequestHandler) -> None:
        for _ in range(pieces):
            handler.wfile.write(piece)
            progress["written"] += len(piece)
        written.set()

    def written_after(seconds: float) -> int:
        time.sleep(seconds)
        return progress["written"]

    server.routes["/large"] = _Route(headers=[("Content-Type", "text/plain")], write=write)
    url = server.page("/", "<p>large</p>")

    with _open_page(session, url) as page:
        page.evaluate("window.pending = fetch('/large'); null")
        assert server.wait_for("/large")
        # The server stops short of the end, and stays there, while no script reads.
        held = written_after(2.0)
        assert written_after(1.0) == held
        assert not written.is_set()
        assert (
            page.evaluate(
                "pending.then(response => response.arrayBuffer()).then(buffer => buffer.byteLength)", timeout=120
            )
            == len(piece) * pieces
        )
        assert written.wait(_TIMEOUT_SECONDS)


def test_a_cross_origin_fetch_reaches_the_page_only_through_the_cors_check(
    session: mitto.Session, server: _PageServer
) -> None:
    with _PageServer() as other:
        other.routes["/open"] = _Route(
            b"open",
            headers=[
                ("Content-Type", "text/plain"),
                ("Access-Control-Allow-Origin", "*"),
                ("Access-Control-Expose-Headers", "X-Shown"),
                ("X-Shown", "1"),
                ("X-Hidden", "1"),
            ],
        )
        other.routes["/closed"] = _Route(b"closed", headers=[("Content-Type", "text/plain")])
        other.routes["OPTIONS /open"] = _Route(
            headers=[("Access-Control-Allow-Origin", "*"), ("Access-Control-Allow-Headers", "X-Custom")]
        )
        url = server.page("/", "<p>cors</p>")

        with _open_page(session, url) as page:
            opened = page.evaluate(
                f"fetch('{other.url('/open')}').then(async response => "
                "[response.type, await response.text(), response.headers.get('x-shown'), response.headers.get('x-hidden'),"
                " response.headers.get('content-type')])",
                timeout=_TIMEOUT_SECONDS,
            )
            closed = page.evaluate(
                f"fetch('{other.url('/closed')}').then(() => 'read', error => String(error))", timeout=_TIMEOUT_SECONDS
            )
            # One that needs a CORS preflight goes once the preflight allows it, and one that it does not allow goes
            # nowhere.
            preflighted = page.evaluate(
                f"fetch('{other.url('/open')}', {{headers: {{'X-Custom': '1'}}}}).then(() => 'read', error => String(error))",
                timeout=_TIMEOUT_SECONDS,
            )
            refused = page.evaluate(
                f"fetch('{other.url('/open')}', {{headers: {{'X-Other': '1'}}}}).then(() => 'read', error => String(error))",
                timeout=_TIMEOUT_SECONDS,
            )

        assert opened == ["cors", "open", "1", None, "text/plain"]
        assert closed == "TypeError: Failed to fetch"
        assert (preflighted, refused) == ("read", "TypeError: Failed to fetch")
        assert len(other.headers_of("/open")) == 2
        preflights = other.headers_of("OPTIONS /open")
        assert [dict(headers)["Access-Control-Request-Headers"] for headers in preflights] == ["x-custom", "x-other"]
        # A cross-origin fetch carries the page's origin, and no cookies, since it asks for none.
        origin = url.removesuffix("/")
        assert ("Origin", origin) in other.headers_of("/open")[0]


def test_request_response_and_headers_behave_as_in_chrome(session: mitto.Session, server: _PageServer) -> None:
    url = server.page("/", "<p>objects</p>")

    with _open_page(session, url) as page:
        script = (
            "(async () => {"
            "  const headers = new Headers([['Accept', 'a'], ['accept', 'b']]);"
            "  headers.append('X-One', ' padded ');"
            "  const request = new Request('/r', {method: 'put', headers: {'Content-Type': 'text/x'}, body: 'b'});"
            "  const made = new Response('made', {status: 202, statusText: 'Made', headers: {'X-M': '1'}});"
            "  const json = Response.json({k: 'v'});"
            "  const errors = [];"
            "  for (const f of [() => headers.append('bad name', 'x'), () => new Request('/r', {method: 'CONNECT'}),"
            "                   () => new Request('/r', {body: 'x'}), () => new Response('x', {status: 99}),"
            "                   () => new Request('http://user:pass@example.com/')]) {"
            "    try { f(); errors.push('none'); } catch (error) { errors.push(String(error)); }"
            "  }"
            "  return [headers.get('ACCEPT'), headers.get('x-one'), headers.has('x-two'), request.method,"
            "          request.url.endsWith('/r'), request.headers.get('content-type'), made.status, made.statusText,"
            "          made.type, await made.text(), json.headers.get('content-type'), await json.text(), errors];"
            "})()"
        )
        result = page.evaluate(script, timeout=_TIMEOUT_SECONDS)
    assert result == [
        "a, b",
        "padded",
        False,
        "PUT",
        True,
        "text/x",
        202,
        "Made",
        "default",
        "made",
        "application/json",
        '{"k":"v"}',
        [
            "TypeError: Failed to execute 'append' on 'Headers': Invalid name",
            "TypeError: Failed to construct 'Request': 'CONNECT' HTTP method is unsupported.",
            "TypeError: Failed to construct 'Request': Request with GET/HEAD method cannot have body.",
            "RangeError: Failed to construct 'Response': The status provided (99) is outside the range [200, 599].",
            "TypeError: Failed to construct 'Request': Request cannot be constructed from a URL that includes "
            "credentials: http://user:pass@example.com/",
        ],
    ]


def test_the_microtasks_of_a_listener_that_the_page_calls_run_before_the_next_listener(
    session: mitto.Session, server: _PageServer
) -> None:
    # The HTML Standard's clean up after running a callback: load comes from the Page, not from a script, so no script
    # runs around its listeners, and each one's microtasks run once it returns.
    url = server.page(
        "/",
        "<script>window.log = [];"
        "addEventListener('load', () => { log.push('first'); Promise.resolve().then(() => log.push('its microtask')); });"
        "addEventListener('load', () => log.push('second'));"
        "</script>",
    )

    with _open_page(session, url) as page:
        assert page.evaluate("log") == ["first", "its microtask", "second"]


def test_a_fetch_that_a_redirect_takes_to_another_origin_passes_the_cors_check_there(
    session: mitto.Session, server: _PageServer
) -> None:
    with _PageServer() as other:
        other.routes["/open"] = _Route(
            b"open", headers=[("Content-Type", "text/plain"), ("Access-Control-Allow-Origin", "*")]
        )
        other.routes["/closed"] = _Route(b"closed", headers=[("Content-Type", "text/plain")])
        server.routes["/to-open"] = _Route(status=302, headers=[("Location", other.url("/open"))])
        server.routes["/to-closed"] = _Route(status=302, headers=[("Location", other.url("/closed"))])
        url = server.page("/", "<p>redirects</p>")

        with _open_page(session, url) as page:
            result = page.evaluate(
                "Promise.all(['/to-open', '/to-closed'].map(path => fetch(path).then("
                "  async response => [response.type, response.redirected, await response.text()],"
                "  error => String(error))))",
                timeout=_TIMEOUT_SECONDS,
            )

    assert result == [["cors", True, "open"], "TypeError: Failed to fetch"]


def test_each_hop_of_a_cross_origin_fetch_passes_the_cors_check_before_it_is_followed(
    session: mitto.Session, server: _PageServer
) -> None:
    # A redirect that fails the check is not followed. One back to the document's origin keeps the response a CORS one,
    # whose origin the redirect from another origin tainted, so it needs Access-Control-Allow-Origin too.
    with _PageServer() as other:
        allow = [("Access-Control-Allow-Origin", "*")]
        server.routes["/allowed"] = _Route(b"allowed", headers=[("Content-Type", "text/plain"), *allow])
        server.routes["/plain"] = _Route(b"plain", headers=[("Content-Type", "text/plain")])
        other.routes["/closed-hop"] = _Route(status=302, headers=[("Location", server.url("/allowed"))])
        other.routes["/to-allowed"] = _Route(status=302, headers=[("Location", server.url("/allowed")), *allow])
        other.routes["/to-plain"] = _Route(status=302, headers=[("Location", server.url("/plain")), *allow])
        url = server.page("/", "<p>hops</p>")

        with _open_page(session, url) as page:
            result = page.evaluate(
                f"Promise.all(['{other.url('/closed-hop')}', '{other.url('/to-allowed')}', '{other.url('/to-plain')}']"
                ".map(target => fetch(target).then(async response => [response.type, await response.text()],"
                " error => String(error))))",
                timeout=_TIMEOUT_SECONDS,
            )

    assert result == ["TypeError: Failed to fetch", ["cors", "allowed"], "TypeError: Failed to fetch"]
    # The redirect that failed the check was not followed.
    assert len(server.headers_of("/allowed")) == 1


def test_a_request_that_needs_a_preflight_at_another_origin_goes_there_only_if_it_allows(
    session: mitto.Session, server: _PageServer
) -> None:
    # A same-origin POST with a line that is not CORS-safelisted needs no preflight, but a hop to another origin does.
    # That one does not allow X-Custom, so the other server gets the preflight and nothing more.
    with _PageServer() as other:
        other.routes["/dest"] = _Route(
            b"dest", headers=[("Content-Type", "text/plain"), ("Access-Control-Allow-Origin", "*")]
        )
        server.routes["/hop"] = _Route(status=307, headers=[("Location", other.url("/dest"))])
        url = server.page("/", "<p>preflight</p>")

        with _open_page(session, url) as page:
            result = page.evaluate(
                "fetch('/hop', {method: 'POST', body: 'x', headers: {'X-Custom': '1'}})"
                ".then(() => 'followed', error => String(error))",
                timeout=_TIMEOUT_SECONDS,
            )

        assert result == "TypeError: Failed to fetch"
        assert other.headers_of("/dest") == []
        assert len(other.headers_of("OPTIONS /dest")) == 1


def test_a_null_body_reads_as_empty_and_stays_unused_after_the_request_ends(
    session: mitto.Session, server: _PageServer
) -> None:
    server.routes["/empty"] = _Route(status=204)
    url = server.page("/", "<p>empty</p>")

    with _open_page(session, url) as page:
        script = (
            "fetch('/empty').then(async response => {"
            "  await new Promise(resolve => setTimeout(resolve, 50));"
            "  return [response.status, await response.text(), response.bodyUsed, await response.text()];"
            "})"
        )
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == [204, "", False, ""]


def test_a_same_origin_fetch_does_not_follow_a_redirect_to_another_origin(
    session: mitto.Session, server: _PageServer
) -> None:
    with _PageServer() as other:
        other.routes["/dest"] = _Route(
            b"dest", headers=[("Content-Type", "text/plain"), ("Access-Control-Allow-Origin", "*")]
        )
        server.routes["/hop"] = _Route(status=302, headers=[("Location", other.url("/dest"))])
        url = server.page("/", "<p>same origin</p>")

        with _open_page(session, url) as page:
            result = page.evaluate(
                "Promise.all([fetch('/hop', {mode: 'same-origin'}), fetch('" + other.url("/dest") + "', "
                "{mode: 'same-origin'})].map(p => p.then(() => 'fetched', error => String(error))))",
                timeout=_TIMEOUT_SECONDS,
            )

        assert result == ["TypeError: Failed to fetch", "TypeError: Failed to fetch"]
        assert other.headers_of("/dest") == []


def test_a_head_request_gives_a_null_body(session: mitto.Session, server: _PageServer) -> None:
    server.routes["/thing"] = _Route(b"", headers=[("Content-Type", "text/plain")])
    url = server.page("/", "<p>head</p>")

    with _open_page(session, url) as page:
        script = (
            "fetch('/thing', {method: 'HEAD'}).then(async response => {"
            "  await new Promise(resolve => setTimeout(resolve, 50));"
            "  return [response.status, await response.text(), response.bodyUsed, await response.text()];"
            "})"
        )
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == [200, "", False, ""]


def test_a_document_resolves_its_urls_against_its_first_base_element_with_an_href_as_chrome_does(
    session: mitto.Session, server: _PageServer
) -> None:
    server.script("/sub/dir/b.js", "log('b ran');")
    server.json("/sub/dir/f.json", "sub")
    url = server.page(
        "/",
        "<script>window.logged = []; function log(x) { logged.push(String(x)); }"
        "window.done = new Promise(resolve => { window.finish = resolve; });</script>"
        "<base id=b0><base id=b1 href='  /sub/dir/  '><base id=b2 href='/other/'>"
        "<script id=b src=b.js></script>"
        "<script>"
        "function attempt(f) { try { f(); } catch (e) { log('threw ' + e); } }"
        "attempt(() => log('baseURI ' + document.baseURI));"
        "attempt(() => log('request ' + new Request('r').url));"
        "const bases = ['b0', 'b1', 'b2'].map(id => document.getElementById(id));"
        "attempt(() => log('base href ' + bases.map(b => b.href).join(' ')));"
        "fetch('f.json').then(r => r.text()).catch(e => 'threw ' + e).then(t => { attempt(() => {"
        " log('fetch ' + t);"
        " bases[1].remove();"
        " log('after remove ' + document.baseURI);"
        " bases[2].setAttribute('href', 'http://[');"
        " log('invalid ' + document.baseURI + ' ' + bases[2].href);"
        " attempt(() => log('invalid request ' + new Request('r').url));"
        " attempt(() => log('absolute request ' + new Request('http://example.test/r').url));"
        " bases[2].setAttribute('href', '');"
        " log('empty ' + document.baseURI + ' ' + bases[2].href);"
        " bases[2].removeAttribute('href');"
        " log('none ' + document.baseURI + ' ' + bases[2].href);"
        " const late = document.createElement('base'); late.href = '/late/'; document.body.append(late);"
        " log('late ' + document.baseURI + ' ' + late.href + ' ' + new Request('l').url);"
        " late.href = 'data:text/plain,x';"
        " log('data ' + document.baseURI + ' ' + late.href);"
        " late.href = 'javascript:void(0)';"
        " log('javascript ' + document.baseURI);"
        " late.remove();"
        " log('removed ' + document.baseURI);"
        " finish(logged);"
        "}); });"
        "</script>",
    )

    with _open_page(session, url) as page:
        origin = server.url("").rstrip("/")
        # As Chrome 157.0.8103.51 gave it, headless on macOS, for the same page.
        assert [line.replace(origin, "{origin}") for line in page.evaluate("done")] == [
            "b ran",
            "baseURI {origin}/sub/dir/",
            "request {origin}/sub/dir/r",
            "base href {origin}/ {origin}/sub/dir/ {origin}/other/",
            "fetch sub",
            "after remove {origin}/other/",
            "invalid about:blank http://[",
            "threw TypeError: Failed to construct 'Request': Failed to parse URL from r",
            "absolute request http://example.test/r",
            "empty {origin}/ {origin}/",
            "none {origin}/ {origin}/",
            "late {origin}/late/ {origin}/late/ {origin}/late/l",
            "data {origin}/ data:text/plain,x",
            "javascript {origin}/",
            "removed {origin}/",
        ]
        assert [message.text for message in page.console_messages if message.level == "error"] == [
            "'data' URLs may not be used as base URLs for a document.",
            "'javascript' URLs may not be used as base URLs for a document.",
        ]
        assert [path for path, _ in server.requests] == ["/", "/sub/dir/b.js", "/sub/dir/f.json"]


def test_a_request_parses_its_url_against_the_base_url_in_utf_8_whatever_the_documents_encoding(
    session: mitto.Session, server: _PageServer
) -> None:
    # Blink's Request parses KURL(base_url, input), whose query is UTF-8, where a document's own URLs, such as a
    # script's src, take its encoding.
    server.routes["/"] = _Route(
        "<base href='/sub/'><p>caf\u00e9</p>".encode("windows-1252"),
        headers=[("Content-Type", "text/html; charset=windows-1252")],
    )
    url = server.url("/")

    with _open_page(session, url) as page:
        assert page.evaluate("[document.characterSet, new Request('r?q=\u00e9').url]") == [
            "windows-1252",
            server.url("/sub/r?q=%C3%A9"),
        ]


def test_a_document_without_a_base_url_keeps_its_encoding_for_an_absolute_url(
    session: mitto.Session, server: _PageServer
) -> None:
    # An invalid base leaves the document with none: Blink's KURL(null base, url, encoding) resolves only an absolute
    # URL, its query still in the document's encoding. What Chrome 157.0.8103.51 asked for, by hand.
    server.routes["/"] = _Route(
        f"<base href='http://['><script src='{server.url('/s.js?q=\u00e9')}'></script>".encode("windows-1252"),
        headers=[("Content-Type", "text/html; charset=windows-1252")],
    )
    server.routes["/s.js?q=%E9"] = _Route(b"window.ran = true;", headers=[("Content-Type", "text/javascript")])
    url = server.url("/")

    with _open_page(session, url) as page:
        assert page.evaluate("[document.baseURI, window.ran]") == ["about:blank", True]
    assert [path for path, _ in server.requests] == ["/", "/s.js?q=%E9"]
