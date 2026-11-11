"""Tests of a Page that loads its document through its Session: Session._open_page() and AsyncSession._open_page(), which
stay private until M10.

A local server serves each page and its scripts, and records what each request carried. Each test goes through both APIs,
and needs a native library with the Page program beside it, as CI's native job installs it on every Host.
"""

import http.server
import socket
import threading
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._errors import PageTimeoutError
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_HTML = "text/html; charset=utf-8"
_JAVASCRIPT = "text/javascript"


@dataclass
class _Route:
    """What the server answers for one path: a status, header lines and a body, or a function that writes the body."""

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    write: Callable[[http.server.BaseHTTPRequestHandler], None] | None = None


class _PageServer:
    """A local server for pages and their scripts, which records the header lines of each request it gets, in order."""

    def __init__(self) -> None:
        self.routes: dict[str, _Route] = {}
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.requested = threading.Condition()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with server.requested:
                    server.requests.append((self.path, list(self.headers.items())))
                    server.requested.notify_all()
                route = server.routes.get(self.path, _Route(b"not here", 404, [("Content-Type", "text/plain")]))
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
                self.wfile.write(route.body)

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
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    return f"http://127.0.0.1:{port}/"


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
