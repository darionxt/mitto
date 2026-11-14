"""Tests of the navigations that a Page's document starts: location, links, a navigation within the document, what the
old document goes through, and the responses that leave it in place, as Chrome 157 has them.

A local server serves each page and records what each request carried. Each test goes through both APIs, and needs a
native library with the Page program beside it, as CI's native job installs it on every Host.
"""

import http.server
import socket
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import unquote

import pytest

import mitto
from mitto import _native
from mitto._errors import ScriptError
from mitto._page import Page, PageNavigation
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_HTML = "text/html; charset=utf-8"

# What the old document's listeners record, in the cookie that the Session keeps across documents.
_RECORD_EVENTS = """
<script>
  window.record = (name) => {
    const seen = (document.cookie.match(/events=([^;]*)/) || [, ''])[1];
    document.cookie = 'events=' + (seen ? seen + ',' : '') + name;
  };
  addEventListener('beforeunload', () => record('beforeunload'));
  addEventListener('pagehide', (e) => record('pagehide ' + e.persisted + ' ' + e.target.nodeName));
  document.addEventListener('visibilitychange', () => record('visibilitychange ' + document.visibilityState));
  addEventListener('unload', () => record('unload'));
</script>
"""


@dataclass
class _Route:
    """What the server answers for one path: a status, header lines and a body. With `hold`, it answers once it is set."""

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: threading.Event | None = None


class _Server:
    """A local server for pages, which records the path and header lines of each request it gets, in order."""

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
                if route.hold is not None:
                    route.hold.wait(_TIMEOUT_SECONDS)
                self.send_response(route.status)
                for name, value in route.headers:
                    self.send_header(name, value)
                if route.status not in (204, 205):
                    self.send_header("Content-Length", str(len(route.body)))
                self.end_headers()
                if route.status not in (204, 205):
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

    def paths(self) -> list[str]:
        with self.requested:
            return [path for path, _ in self.requests]

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        """Return the header lines of each request for `path`, in the order they came."""
        with self.requested:
            return [headers for requested, headers in self.requests if requested == path]

    def wait_for(self, path: str, count: int = 1) -> bool:
        """Return whether `count` requests for `path` have come, waiting for them up to the timeout."""
        with self.requested:
            return self.requested.wait_for(
                lambda: sum(requested == path for requested, _ in self.requests) >= count, _TIMEOUT_SECONDS
            )

    def __enter__(self) -> "_Server":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        for route in self.routes.values():
            if route.hold is not None:
                route.hold.set()
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_Server]:
    with _Server() as page_server:
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


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _next(page: Page) -> PageNavigation:
    return page.wait_for_navigation(timeout=_TIMEOUT_SECONDS)


def _events(session: mitto.Session) -> list[str]:
    """Return what the old documents recorded in the Session's cookie, in order."""
    for cookie in session.cookies:
        if cookie.name == "events":
            return unquote(cookie.value).split(",")
    return []


def _without(lines: list[tuple[str, str]], *names: str) -> list[tuple[str, str]]:
    return [(name, value) for name, value in lines if name.lower() not in {n.lower() for n in names}]


def test_a_document_navigates_through_location_as_a_navigation_that_it_started(
    session: mitto.Session, server: _Server
) -> None:
    second = server.page("/second", "<title>Second</title><p>second")
    first = server.page("/first", "<title>First</title><p>first")

    with _open_page(session, first) as page:
        assert page.evaluate("location.href = '/second'; 'set'") == "set"
        assert _next(page) == PageNavigation(url=second, status=200)
        assert page.url == second
        assert page.evaluate("[document.URL, document.title, location.href, document.referrer]") == [
            second,
            "Second",
            second,
            first,
        ]
    session.get(second, kind="navigation", initiator=first)

    # The document is its Initiator, as for a caller's navigation from it, with no Sec-Fetch-User: no user activated
    # the page.
    page_lines, caller_lines = server.headers_of("/second")
    assert ("Sec-Fetch-Site", "same-origin") in page_lines
    assert ("Referer", first) in page_lines
    assert "Sec-Fetch-User" not in dict(page_lines)
    assert page_lines == _without(caller_lines, "Sec-Fetch-User")


def test_assign_and_replace_navigate_and_reload_validates_the_cache(session: mitto.Session, server: _Server) -> None:
    server.page("/a", "<title>A</title>")
    server.page("/b", "<title>B</title>")
    start = server.page("/", "<title>Start</title>")

    with _open_page(session, start) as page:
        page.evaluate("location.assign('/a')")
        assert _next(page).url == server.url("/a")
        page.evaluate("location.replace('b')")
        assert _next(page).url == server.url("/b")
        page.evaluate("location.reload()")
        assert _next(page).url == server.url("/b")
        assert page.evaluate("document.title") == "B"

    # A reload asks the server again, which //net says with Cache-Control: max-age=0, and comes from the document it
    # reloads, as a client redirect does.
    first, reloaded = server.headers_of("/b")
    assert "Cache-Control" not in dict(first)
    assert dict(reloaded)["Cache-Control"] == "max-age=0"
    assert (dict(first)["Referer"], dict(reloaded)["Referer"]) == (server.url("/a"), server.url("/b"))
    assert _without(reloaded, "Cache-Control", "Referer") == _without(first, "Referer")


def test_the_old_document_unloads_before_the_new_one_takes_its_place(session: mitto.Session, server: _Server) -> None:
    second = server.page(
        "/second",
        "<script>window.seen = [];"
        "addEventListener('load', () => seen.push('load'));"
        "addEventListener('pageshow', (e) => seen.push('pageshow ' + e.persisted + ' ' + e.target.nodeName));"
        "</script>",
    )
    first = server.page("/first", f"<body>{_RECORD_EVENTS}")

    with _open_page(session, first) as page:
        page.evaluate("location.href = '/second'")
        assert _next(page).url == second
        assert page.evaluate("seen") == ["load", "pageshow false #document"]
        # The seed of the Session's Persona deprecates unload on every page of HTTP or HTTPS, so the listener was
        # refused, as Chrome refuses it, with the console's word.
        assert page.console_messages[0] == _native.ConsoleMessage(
            level="error", text="Permissions policy violation: unload is not allowed in this document."
        )
    assert _events(session) == ["beforeunload", "pagehide false #document", "visibilitychange hidden"]


def test_a_navigation_within_the_document_changes_its_url_with_popstate_then_hashchange(
    session: mitto.Session, server: _Server
) -> None:
    url = server.page(
        "/",
        "<script>window.seen = [];"
        "addEventListener('popstate', (e) => seen.push('popstate ' + e.state));"
        "addEventListener('hashchange', (e) => seen.push('hashchange ' + e.oldURL + ' ' + e.newURL));"
        "</script>",
    )

    with _open_page(session, url) as page:
        # The URL changes at once, popstate fires at once, and hashchange from a later task.
        assert page.evaluate("location.hash = 'part'; [location.href, seen.slice()]") == [
            url + "#part",
            ["popstate null"],
        ]
        assert _next(page) == PageNavigation(url=url + "#part", status=0, same_document=True)
        assert page.url == url + "#part"
        assert page.evaluate("new Promise(r => setTimeout(() => r(seen), 0))") == [
            "popstate null",
            f"hashchange {url} {url}#part",
        ]
        # The same fragment again navigates nowhere.
        assert page.evaluate("location.hash = '#part'; seen.length") == 2
    assert server.paths() == ["/"]


def test_a_link_navigates_when_click_activates_it_but_a_new_window_does_not_open(
    session: mitto.Session, server: _Server
) -> None:
    second = server.page("/second", "<title>Second</title>")
    first = server.page(
        "/first",
        "<a id=link href=/second>next</a><a id=blank href=/other target=_blank>other</a>"
        "<a id=download href=/file download>file</a>",
    )

    with _open_page(session, first) as page:
        # A window that only the user's activation opens does not open, and a download that the document can read
        # starts no navigation.
        page.evaluate("document.getElementById('blank').click(); document.getElementById('download').click()")
        page.evaluate("document.getElementById('link').click()")
        assert _next(page).url == second
    assert server.paths() == ["/first", "/second"]
    (lines,) = server.headers_of("/second")
    assert ("Sec-Fetch-Site", "same-origin") in lines
    assert "Sec-Fetch-User" not in dict(lines)


def test_a_204_and_a_download_keep_the_document(session: mitto.Session, server: _Server) -> None:
    server.routes["/nothing"] = _Route(status=204)
    server.routes["/file"] = _Route(
        b"x" * 200_000, headers=[("Content-Type", "text/html"), ("Content-Disposition", "attachment; name=file.txt")]
    )
    server.routes["/zip"] = _Route(b"PK", headers=[("Content-Type", "application/zip")])
    url = server.page("/", "<body>still here")

    with _open_page(session, url) as page:
        page.evaluate("window.marker = 1; location.href = '/nothing'")
        assert _next(page) == PageNavigation(url=server.url("/nothing"), status=204, kept_document=True)
        page.evaluate("location.href = '/file'")
        assert _next(page) == PageNavigation(url=server.url("/file"), status=200, kept_document=True)
        page.evaluate("location.href = '/zip'")
        assert _next(page) == PageNavigation(url=server.url("/zip"), status=200, kept_document=True)
        assert page.url == url
        assert page.evaluate("[window.marker, document.body.textContent]") == [1, "still here"]


def test_a_navigation_that_starts_while_the_document_loads_stops_its_parser(
    session: mitto.Session, server: _Server
) -> None:
    second = server.page("/second", "<title>Second</title>")
    first = server.page(
        "/first",
        "<script>location.href = '/second';</script><script>document.cookie = 'parsed=on';</script>",
    )

    with _open_page(session, first) as page:
        assert page.url == second
        assert page.evaluate("document.title") == "Second"
    assert all(cookie.name != "parsed" for cookie in session.cookies)


def test_a_page_opens_when_its_document_stops_loading_for_a_navigation_that_keeps_it(
    session: mitto.Session, server: _Server
) -> None:
    server.routes["/nothing"] = _Route(status=204)
    url = server.page("/", "<script>location.href = '/nothing';</script><p id=after>after")

    # The navigation stopped the parser, so the load event never fires, and the page opens once the navigation ends.
    with _open_page(session, url) as page:
        assert page.url == url
        assert _next(page) == PageNavigation(url=server.url("/nothing"), status=204, kept_document=True)
        assert page.evaluate("[document.readyState, document.getElementById('after')]") == ["complete", None]


def test_a_navigation_ends_once_its_own_document_has_loaded_and_not_the_one_it_left(
    session: mitto.Session, server: _Server
) -> None:
    # The second document's script waits on the server, so its load comes well after the first document's.
    hold = threading.Event()
    server.routes["/slow.js"] = _Route(b"window.slow = 1;", headers=[("Content-Type", "text/javascript")], hold=hold)
    second = server.page("/second", "<title>Second</title><script src=/slow.js></script>")
    # The load listener runs on until the navigation's response has come, so the first document's load ends after it.
    first = server.page(
        "/first",
        "<script>addEventListener('load', () => {"
        "  location.href = '/second'; const end = Date.now() + 1000; while (Date.now() < end);"
        "});</script>",
    )
    release = threading.Timer(2.0, hold.set)
    release.start()

    with _open_page(session, first) as page:
        assert _next(page).url == second
        assert page.evaluate("[document.title, document.readyState, window.slow]") == ["Second", "complete", 1]
    release.cancel()


def test_a_javascript_url_runs_from_a_task_and_navigates_nowhere(session: mitto.Session, server: _Server) -> None:
    url = server.page("/", "<body>")

    with _open_page(session, url) as page:
        assert page.evaluate("location.href = 'javascript:window.ran = 1%2B1'; typeof window.ran") == "undefined"
        assert page.evaluate("new Promise(r => setTimeout(() => r(window.ran), 0))") == 2
        assert page.url == url
    assert server.paths() == ["/"]


def test_location_refuses_a_url_that_does_not_resolve(session: mitto.Session, server: _Server) -> None:
    url = server.page("/", "<body>")

    with _open_page(session, url) as page:
        with pytest.raises(ScriptError) as raised:
            page.evaluate("location.href = 'http://['")
        assert str(raised.value) == (
            "Uncaught SyntaxError: Failed to set the 'href' property on 'Location': 'http://[/' is not a valid URL."
        )
        with pytest.raises(ScriptError) as raised:
            page.evaluate("location.protocol = 'a b'")
        assert str(raised.value).endswith(": 'a b' is an invalid protocol.")


def test_a_navigation_to_a_url_longer_than_chrome_carries_starts_nothing(
    session: mitto.Session, server: _Server
) -> None:
    # Chrome's browser gets no URL of more than 2 MiB and goes to about:blank#blocked, which a Page does not, so the
    # document goes on loading, as no navigation started.
    url = server.page("/", "<script>location.href = '/' + 'x'.repeat(2 * 1024 * 1024);</script><title>Here</title>")

    with _open_page(session, url) as page:
        assert page.evaluate("location.href = '/' + 'y'.repeat(2 * 1024 * 1024); document.title") == "Here"
        assert page.evaluate("new Promise(r => setTimeout(r, 100)).then(() => document.URL)") == url
    assert server.paths() == ["/"]


def test_a_documents_referrer_is_the_referer_that_its_navigation_sent_last(
    session: mitto.Session, server: _Server
) -> None:
    done = server.page("/done", "<title>Done</title>")
    server.routes["/bounce"] = _Route(status=302, headers=[("Location", done)])
    first = server.page("/deep/path?token=1", "<title>First</title>")
    bounce = server.url("/bounce").replace("127.0.0.1", "localhost")

    with _open_page(session, first) as page:
        page.evaluate(f"location.href = '{bounce}'")
        assert _next(page).url == done
        # The hop to another origin cut the Referer to the document's origin, and the hop back does not give the path
        # back, as in Chrome.
        assert page.evaluate("document.referrer") == server.url("/")
    ((bounce_lines,), (done_lines,)) = server.headers_of("/bounce"), server.headers_of("/done")
    assert dict(bounce_lines)["Referer"] == dict(done_lines)["Referer"] == server.url("/")


def test_a_navigation_that_fails_raises_its_error_and_keeps_the_document(
    session: mitto.Session, server: _Server
) -> None:
    url = server.page("/", "<body>kept")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        refused = f"http://127.0.0.1:{listener.getsockname()[1]}/"

    with _open_page(session, url) as page:
        page.evaluate(f"location.href = '{refused}'")
        with pytest.raises(mitto.ConnectionFailedError):
            _next(page)
        assert page.url == url
        assert page.evaluate("document.body.textContent") == "kept"


def test_a_new_navigation_takes_the_place_of_one_that_waits_and_the_same_one_is_dropped(
    session: mitto.Session, server: _Server
) -> None:
    hold = threading.Event()
    server.routes["/slow"] = _Route(b"<title>Slow</title>", headers=[("Content-Type", _HTML)], hold=hold)
    fast = server.page("/fast", "<title>Fast</title>")
    url = server.page("/", "<body>")

    with _open_page(session, url) as page:
        # The same navigation again, within three seconds, is dropped.
        page.evaluate("location.href = '/slow'; location.href = '/slow'")
        assert server.wait_for("/slow")
        page.evaluate("location.href = '/fast'")
        with pytest.raises(mitto.RequestError, match="ERR_ABORTED"):
            _next(page)
        assert _next(page).url == fast
        hold.set()
    assert server.paths().count("/slow") == 1


def test_a_navigation_whose_response_came_ends_when_another_takes_its_place_before_its_document(
    session: mitto.Session, server: _Server
) -> None:
    server.page("/a", "<title>A</title>")
    server.routes["/nothing"] = _Route(status=204)
    url = server.page("/", "<title>Start</title>")

    with _open_page(session, url) as page:
        # The script runs on until the first navigation's response has come, then starts one that keeps the document.
        page.evaluate(
            "location.href = '/a'; const end = Date.now() + 1000; while (Date.now() < end); location.href = '/nothing'"
        )
        with pytest.raises(mitto.RequestError, match="ERR_ABORTED"):
            _next(page)
        assert _next(page) == PageNavigation(url=server.url("/nothing"), status=204, kept_document=True)
        assert page.url == url
        assert page.evaluate("[document.title, location.href]") == ["Start", url]


def test_an_opening_navigation_to_a_204_leaves_the_page_at_about_blank(session: mitto.Session, server: _Server) -> None:
    server.routes["/nothing"] = _Route(status=204)

    with _open_page(session, server.url("/nothing")) as page:
        assert page.url == "about:blank"
        assert page.evaluate("document.URL") == "about:blank"


def test_a_meta_refresh_navigates_once_the_document_has_loaded(session: mitto.Session, server: _Server) -> None:
    second = server.page("/second", "<title>Second</title>")
    first = server.page("/first", "<meta http-equiv=refresh content=\"0; URL='/second'\"><title>First</title>")

    with _open_page(session, first) as page:
        assert _next(page).url == second
        assert page.evaluate("document.title") == "Second"
    (lines,) = server.headers_of("/second")
    assert ("Sec-Fetch-Site", "same-origin") in lines
    assert ("Referer", first) in lines
    assert "Sec-Fetch-User" not in dict(lines)


def test_a_refresh_header_navigates_and_one_to_the_documents_own_url_reloads(
    session: mitto.Session, server: _Server
) -> None:
    # The second page asks for a refresh to its own URL once, through a meta element that its script inserts.
    second = server.page(
        "/second",
        "<script>if (!document.cookie.includes('again=1')) { document.cookie = 'again=1';"
        "const meta = document.createElement('meta'); meta.httpEquiv = 'refresh'; meta.content = '0';"
        "document.head.append(meta); }</script>",
    )
    first = server.page("/first", "<title>First</title>", headers=[("Refresh", "0;url=/second")])

    with _open_page(session, first) as page:
        assert _next(page).url == second
        # A refresh to its own URL reloads it, which validates the cache.
        assert _next(page).url == second
    first_load, reload = server.headers_of("/second")
    assert "Cache-Control" not in dict(first_load)
    assert dict(reload)["Cache-Control"] == "max-age=0"


def test_a_refresh_waits_for_its_delay_and_the_shortest_wins(session: mitto.Session, server: _Server) -> None:
    server.page("/late", "<title>Late</title>")
    server.page("/soon", "<title>Soon</title>")
    first = server.page(
        "/first",
        "<meta http-equiv=refresh content='5; url=/late'><meta http-equiv=refresh content='1.9;/soon'>",
    )

    with _open_page(session, first) as page:
        loaded = time.monotonic()
        assert _next(page).url == server.url("/soon")
        assert time.monotonic() - loaded >= 0.9
    assert "/late" not in server.paths()


def test_a_refresh_to_a_javascript_url_is_refused(session: mitto.Session, server: _Server) -> None:
    url = server.page("/", "<meta http-equiv=refresh content='0; url=javascript:alert(1)'>")

    with _open_page(session, url) as page:
        assert page.evaluate("new Promise(r => setTimeout(r, 100)).then(() => document.URL)") == url
        assert _native.ConsoleMessage(level="error", text=f"Refused to refresh {url} to a javascript: URL") in (
            page.console_messages
        )


def test_window_open_navigates_this_window_for_self_and_opens_no_new_one(
    session: mitto.Session, server: _Server
) -> None:
    second = server.page("/second", "<title>Second</title>")
    first = server.page("/first", "<title>First</title>")

    with _open_page(session, first) as page:
        # No user activated the page, so the pop-up blocker refuses a new window.
        assert page.evaluate("[window.open('/never'), window.open('/never', 'name'), window.open()]") == [
            None,
            None,
            None,
        ]
        with pytest.raises(ScriptError) as raised:
            page.evaluate("window.open('http://[')")
        assert str(raised.value) == (
            "Uncaught SyntaxError: Failed to execute 'open' on 'Window': Unable to open a window with invalid URL"
            " 'http://[/'.\n"
        )
        assert page.evaluate("window.open('/second', '_self', 'noopener') === window") is True
        assert _next(page).url == second
    assert "/never" not in server.paths()


def test_navigation_timing_says_a_reload_and_gives_the_unload_times_of_a_document_of_its_origin(
    session: mitto.Session, server: _Server
) -> None:
    server.page("/second", "<title>Second</title>")
    first = server.page("/first", "<title>First</title>")
    server.routes["/bounce"] = _Route(status=302, headers=[("Location", first)])
    bounce = server.url("/bounce").replace("127.0.0.1", "localhost")
    timing = (
        "(() => { const entry = performance.getEntriesByType('navigation')[0];"
        "return [performance.navigation.type, entry.type, entry.unloadEventStart > 0,"
        "entry.unloadEventStart <= entry.unloadEventEnd, performance.timing.unloadEventStart > 0]; })()"
    )

    with _open_page(session, first) as page:
        # The first document comes from no document, so it has no unload times.
        assert page.evaluate(timing) == [0, "navigate", False, True, False]
        page.evaluate("location.href = '/second'")
        _next(page)
        assert page.evaluate(timing) == [0, "navigate", True, True, True]
        page.evaluate("location.reload()")
        _next(page)
        assert page.evaluate(timing) == [1, "reload", True, True, True]
        # A redirect through another origin hides them, though the navigation comes back to the old document's origin.
        page.evaluate(f"location.href = '{bounce}'")
        assert _next(page).url == first
        assert page.evaluate(timing) == [0, "navigate", False, True, False]


# What the Navigation API's events say, as each fires, and the outcome of a navigate() or reload().
_NAVIGATION_EVENTS = """
<script>
  window.seen = [];
  const entry = (e) => e && [e.url, e.index, e.sameDocument];
  navigation.addEventListener('navigate', (e) => seen.push([
    'navigate', e.navigationType, e.destination.url, e.destination.sameDocument, e.canIntercept, e.hashChange,
    e.cancelable, e.userInitiated, e.info ?? null, e.sourceElement && e.sourceElement.id,
  ]));
  navigation.addEventListener('currententrychange', (e) => seen.push(['currententrychange', entry(e.from)]));
  navigation.addEventListener('navigatesuccess', () => seen.push(['navigatesuccess']));
  navigation.addEventListener('navigateerror', (e) => seen.push([
    'navigateerror', e.error.name + ': ' + e.error.message, e.message,
  ]));
  addEventListener('popstate', () => seen.push(['popstate']));
  addEventListener('hashchange', () => seen.push(['hashchange']));
  window.later = () => new Promise((resolve) => setTimeout(resolve, 0)).then(() => seen.splice(0));
  window.settled = (result) => Promise.allSettled([result.committed, result.finished]).then((outcomes) =>
    outcomes.map((o) => o.status === 'fulfilled' ? 'fulfilled' : o.reason.name + ': ' + o.reason.message));
</script>
"""

_ABORTED = "AbortError: Navigation was aborted"


def test_the_navigate_event_comes_first_and_canceling_it_stops_the_navigation(
    session: mitto.Session, server: _Server
) -> None:
    url = server.page("/", f"<a id=link href=/elsewhere>elsewhere</a>{_NAVIGATION_EVENTS}")

    with _open_page(session, url) as page:
        # A navigation within the document fires navigate, then changes the current entry, then fires popstate; it
        # succeeds later, before hashchange.
        assert page.evaluate("location.hash = 'part'; seen.splice(0)") == [
            ["navigate", "push", url + "#part", True, True, True, True, False, None, None],
            ["currententrychange", [url, 0, True]],
            ["popstate"],
        ]
        assert page.evaluate("later()") == [["navigatesuccess"], ["hashchange"]]
        assert page.evaluate("navigation.entries().map((e) => e.url)") == [url, url + "#part"]
        assert _next(page).url == url + "#part"

        # Canceling navigate stops a navigation to another document before it asks the server, and aborts it, as
        # navigateerror and the promises of navigate() say.
        page.evaluate("navigation.addEventListener('navigate', (e) => e.preventDefault())")
        assert page.evaluate("location.href = '/never'; seen.splice(0)") == [
            ["navigate", "push", server.url("/never"), False, True, False, True, False, None, None],
            ["navigateerror", _ABORTED, "Uncaught "],
        ]
        assert page.evaluate("settled(navigation.navigate('/never', {info: 'given'}))") == [_ABORTED, _ABORTED]
        assert page.evaluate("seen[0][8]") == "given"
        page.evaluate("seen.length = 0; document.getElementById('link').click()")
        assert page.evaluate("seen.splice(0)") == [
            ["navigate", "push", server.url("/elsewhere"), False, True, False, True, False, None, "link"],
            ["navigateerror", _ABORTED, "Uncaught "],
        ]
        assert page.evaluate("location.href") == url + "#part"
    assert server.paths() == ["/"]


def test_intercept_makes_a_navigation_within_the_document_that_ends_with_its_handlers(
    session: mitto.Session, server: _Server
) -> None:
    url = server.page("/", _NAVIGATION_EVENTS)
    app = server.url("/app?view=1")

    with _open_page(session, url) as page:
        page.evaluate(
            "window.release = null;"
            "navigation.addEventListener('navigate', (e) => e.intercept({handler: () => {"
            "  seen.push(['handler', location.href, navigation.transition.navigationType, navigation.transition.from.url]);"
            "  return new Promise((resolve) => { release = resolve; });"
            "}}), {once: true});"
            "window.result = navigation.navigate('/app?view=1', {state: {view: 1}, info: 'from the page'}); 0"
        )
        # It commits at once, with no request, and the handlers run once it has.
        assert _next(page) == PageNavigation(url=app, status=0, same_document=True)
        assert page.url == app
        assert page.evaluate("seen.splice(0)") == [
            ["navigate", "push", app, False, True, False, True, False, "from the page", None],
            ["currententrychange", [url, 0, True]],
            ["handler", app, "push", url],
        ]
        assert page.evaluate(
            "result.committed.then((entry) => [entry === navigation.currentEntry, entry.index, entry.getState()])"
        ) == [True, 1, {"view": 1}]
        # It ends once the handler's promise has settled.
        assert page.evaluate("later().then((events) => [events, navigation.transition !== null])") == [[], True]
        assert page.evaluate("release(); result.finished.then(() => [seen.splice(0), navigation.transition])") == [
            [["navigatesuccess"]],
            None,
        ]

        # A handler's rejection fails the navigation once it has committed: here one that replaces the current entry.
        page.evaluate(
            "navigation.addEventListener('navigate', (e) => e.intercept({"
            "  handler: () => Promise.reject(new TypeError('no view')),"
            "}), {once: true}); 0"
        )
        assert page.evaluate("settled(navigation.navigate('/app?view=2', {history: 'replace'}))") == [
            "fulfilled",
            "TypeError: no view",
        ]
        assert page.evaluate("seen.splice(0)") == [
            ["navigate", "replace", server.url("/app?view=2"), False, True, False, True, False, None, None],
            ["currententrychange", [app, -1, True]],
            ["navigateerror", "TypeError: no view", "Uncaught TypeError: no view"],
        ]
        assert page.evaluate("navigation.entries().map((e) => e.url)") == [url, server.url("/app?view=2")]
        assert _next(page) == PageNavigation(url=server.url("/app?view=2"), status=0, same_document=True)

        # A navigation that starts before an intercepted one has ended aborts it.
        assert page.evaluate(
            "navigation.addEventListener('navigate', (e) => {"
            "  window.signal = e.signal; e.intercept({handler: () => new Promise(() => {})});"
            "}, {once: true});"
            "window.outcome = settled(navigation.navigate('/app?view=3')); location.hash = 'other';"
            "[seen.splice(0), signal.aborted, signal.reason.message]"
        ) == [
            [
                ["navigate", "push", server.url("/app?view=3"), False, True, False, True, False, None, None],
                ["currententrychange", [server.url("/app?view=2"), 1, True]],
                ["navigateerror", _ABORTED, "Uncaught "],
                ["navigate", "push", server.url("/app?view=3#other"), True, True, True, True, False, None, None],
                ["currententrychange", [server.url("/app?view=3"), 2, True]],
                ["popstate"],
            ],
            True,
            "Navigation was aborted",
        ]
        assert page.evaluate("outcome.then((s) => later().then((events) => [s, events]))") == [
            ["fulfilled", _ABORTED],
            [["navigatesuccess"], ["hashchange"]],
        ]
    assert server.paths() == ["/"]


def test_navigate_refuses_what_chrome_refuses(session: mitto.Session, server: _Server) -> None:
    url = server.page("/", _NAVIGATION_EVENTS)
    other_origin = url.replace("127.0.0.1", "localhost")

    with _open_page(session, url) as page:
        assert page.evaluate(
            "Promise.all([navigation.navigate('http://['), navigation.navigate('javascript:void 0'),"
            "navigation.navigate('/never', {state: () => 1})].map(settled))"
        ) == [
            ["SyntaxError: Invalid URL 'http://[/'."] * 2,
            ["NotSupportedError: The javascript: protocol is not supported"] * 2,
            ["DataCloneError: Failed to execute 'navigate' on 'Navigation': () => 1 could not be cloned."] * 2,
        ]
        assert page.evaluate("seen.length") == 0

        # A document may not intercept a navigation to another origin.
        assert page.evaluate(
            "navigation.addEventListener('navigate', (e) => {"
            "  try { e.intercept(); } catch (error) { seen.push([error.name, error.message]); }"
            "  e.preventDefault();"
            "}, {once: true});"
            f"location.href = '{other_origin}'; seen.splice(0)"
        ) == [
            ["navigate", "push", other_origin, False, False, False, True, False, None, None],
            [
                "SecurityError",
                f"Failed to execute 'intercept' on 'NavigateEvent': A navigation with URL '{other_origin}' cannot be"
                f" intercepted by in a window with origin '{url.rstrip('/')}' and URL '{url}'.",
            ],
            ["navigateerror", _ABORTED, "Uncaught "],
        ]
    assert server.paths() == ["/"]


def test_a_new_document_keeps_the_entries_of_its_origin_with_how_it_came(
    session: mitto.Session, server: _Server
) -> None:
    second = server.page("/second", "<title>Second</title>")
    third = server.page("/third", "<title>Third</title>")
    first = server.page("/first", "<a id=link href=/third>third</a>")
    other_origin = second.replace("127.0.0.1", "localhost")
    activation = (
        "(() => { const a = navigation.activation;"
        "return [a.navigationType, a.entry === navigation.currentEntry, a.from && [a.from.url, a.from.index,"
        "a.from.sameDocument], a.from === a.entry, navigation.entries().map((e) => [e.url, e.sameDocument]),"
        "navigation.currentEntry.getState() ?? null]; })()"
    )

    with _open_page(session, first) as page:
        # The first document takes the place of the initial about:blank one.
        assert page.evaluate(activation) == ["replace", True, None, False, [[first, True]], None]
        page.evaluate("navigation.navigate('/second', {state: {kept: false}}); 0")
        assert _next(page).url == second
        # Chrome 157 gives no state of a navigate() to the entry of the document that it loads.
        assert page.evaluate(activation) == [
            "push",
            True,
            [first, 0, False],
            False,
            [[first, False], [second, True]],
            None,
        ]
        page.evaluate("navigation.reload(); 0")
        assert _next(page).url == second
        assert page.evaluate(activation) == [
            "reload",
            True,
            [second, 1, True],
            True,
            [[first, False], [second, True]],
            None,
        ]
        page.evaluate("location.replace('/first')")
        assert _next(page).url == first
        assert page.evaluate(activation) == [
            "replace",
            True,
            [second, -1, False],
            False,
            [[first, False], [first, True]],
            None,
        ]
        page.evaluate("document.getElementById('link').click()")
        assert _next(page).url == third
        assert page.evaluate(activation) == [
            "push",
            True,
            [first, 1, False],
            False,
            [[first, False], [first, False], [third, True]],
            None,
        ]
        # A document of another origin sees its own entry alone.
        page.evaluate(f"location.href = '{other_origin}'")
        assert _next(page).url == other_origin
        assert page.evaluate(activation) == ["push", True, None, False, [[other_origin, True]], None]
