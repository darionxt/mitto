"""Tests of a Page's back/forward cache: which documents it keeps as a navigation leaves them, what they hear as they go
in and come back, what runs while they are kept, and what makes them go, as Chrome 157 has it. Each expected value is
what Chrome for Testing 157.0.8103.79 gave for the same pages and scripts in a new tab.

A local server serves each page. Each test goes through both APIs, and needs a native library with the Page program
beside it, as CI's native job installs it on every Host.
"""

import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page, PageNavigation
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, Route, without_favicon

pytestmark = pytest.mark.usefixtures("both_apis")

_NO_STORE = [("Cache-Control", "no-store")]
# A page that logs what it hears of its lifecycle, in order, and keeps the persisted flag of each pagehide in its
# sessionStorage, which outlives it.
_LOGGING = """<script>
window.log = [];
addEventListener('pagehide', e => {
  log.push('pagehide:' + e.persisted);
  sessionStorage.hide = (sessionStorage.hide || '') + e.persisted + ',';
});
addEventListener('pageshow', e => log.push('pageshow:' + e.persisted));
document.addEventListener('visibilitychange', () => log.push('visibilitychange:' + document.visibilityState));
document.addEventListener('freeze', () => log.push('freeze'));
document.addEventListener('resume', () => log.push('resume'));
</script>"""
# What a kept page hears as it goes into the cache and comes back, after the pageshow of its load.
_KEPT_AND_BACK = [
    "pageshow:false",
    "pagehide:true",
    "visibilitychange:hidden",
    "freeze",
    "resume",
    "visibilitychange:visible",
    "pageshow:true",
]


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    with RecordingServer() as page_server:
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
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _next_document(page: Page) -> PageNavigation:
    """Return the next navigation to another document, past those within the document before it."""
    while (navigation := page.wait_for_navigation(timeout=TIMEOUT_SECONDS)).same_document:
        pass
    return navigation


def _wait(page: Page, milliseconds: int) -> None:
    """Let the page's tasks run for `milliseconds`."""
    page.evaluate(f"new Promise(r => setTimeout(r, {milliseconds}))")


def test_a_page_that_a_traversal_leaves_comes_back_as_it_was(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    server.route("/after", b"", "text/plain")
    state = (
        "[log, window.marker, performance.getEntriesByType('navigation')[0].type,"
        " navigation.activation.navigationType, history.length, document.visibilityState,"
        " navigation.entries().length, new URL(navigation.activation.from.url).pathname,"
        " navigation.activation.entry === navigation.currentEntry]"
    )

    with _open_page(session, first) as page:
        page.evaluate("setTimeout(() => log.push('timeout'), 500); window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page) == PageNavigation(url=second, status=200)
        # The timer's time comes while the cache keeps the page, frozen.
        _wait(page, 900)
        page.evaluate("history.back()")
        # The document comes back with no request, as it was, and thaws: its timer runs once it shows again. Its
        # navigation's entry is the one of its load, and the Navigation API says a traversal activated it.
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate(state) == [
            [*_KEPT_AND_BACK, "timeout"],
            1,
            "navigate",
            "traverse",
            2,
            "visible",
            2,
            "/b",
            True,
        ]
        # The Session knows it as the top-level document again.
        page.evaluate("fetch('/after').then(r => r.text())")
    assert without_favicon(server.paths()) == ["/a", "/b", "/after"]
    (after,) = server.headers_of("/after")
    assert ("Referer", first) in after


def test_a_no_store_page_comes_back_unless_a_cookie_of_its_url_changed(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/ns", "<title>NS</title>" + _LOGGING, _NO_STORE)
    second = server.page("/b", "<title>B</title>")
    third = server.page("/c", "<title>C</title>")

    with _open_page(session, first) as page:
        page.evaluate("window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("[window.marker, log]") == [1, _KEPT_AND_BACK]
        page.evaluate("location.href = '/c'; 0")
        assert _next_document(page).url == third
        # The cache keeps it again, but a cookie of its URL that changes keeps it from coming back, as at Chrome's
        # default level, restore-unless-cookie-change: the traversal asks the server again.
        page.evaluate("document.cookie = 'c=1; path=/'; history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("[window.marker ?? null, sessionStorage.hide, log]") == [
            None,
            "true,true,",
            ["pageshow:false"],
        ]
    assert len(server.headers_of("/ns")) == 2


def test_a_no_store_page_whose_script_had_a_no_store_response_is_not_kept(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/ns", "<title>NS</title>" + _LOGGING, _NO_STORE)
    second = server.page("/b", "<title>B</title>")
    server.route("/fetched", b"", "text/plain", _NO_STORE)

    with _open_page(session, first) as page:
        page.evaluate(
            "fetch('/fetched').then(r => r.text()).then(() => { window.marker = 1; location.href = '/b'; return 0; })"
        )
        assert _next_document(page).url == second
        # It unloads, as a page that the cache does not keep does.
        assert page.evaluate("sessionStorage.hide") == "false,"
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("[window.marker ?? null, log]") == [None, ["pageshow:false"]]
    assert len(server.headers_of("/ns")) == 2


def test_a_traversal_into_the_browsing_instance_of_the_page_it_leaves_does_not_keep_that_page(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/ns", "<title>NS</title>", _NO_STORE)
    second = server.page("/b", "<title>B</title>" + _LOGGING)
    third = server.page("/c", "<title>C</title>" + _LOGGING)
    server.route("/fetched", b"", "text/plain", _NO_STORE)

    with _open_page(session, first) as page:
        # The cache keeps no such first page, so the second shares its browsing instance.
        page.evaluate("fetch('/fetched').then(r => r.text()).then(() => { location.href = '/b'; return 0; })")
        assert _next_document(page).url == second
        page.evaluate("window.marker = 1; history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        # The traversal's document went into the browsing instance of its entry, the second page's, which unloaded,
        # as Chrome does for kBrowsingInstanceNotSwapped.
        assert page.evaluate("sessionStorage.hide") == "false,"
        page.evaluate("history.forward()")
        assert _next_document(page) == PageNavigation(url=second, status=200)
        assert page.evaluate("[window.marker ?? null, log]") == [None, ["pageshow:false"]]
        # A navigation of its own gives the next page a browsing instance of its own, and the cache keeps the second.
        page.evaluate("location.href = '/c'; 0")
        assert _next_document(page).url == third
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=second, status=200, restored=True)
        assert page.evaluate("[log, sessionStorage.hide]") == [_KEPT_AND_BACK, "false,true,true,"]


def test_a_page_of_another_status_is_not_kept(session: mitto.Session, server: RecordingServer) -> None:
    start = server.page("/start", "<title>Start</title>")
    server.routes["/missing"] = Route(
        ("<title>Missing</title>" + _LOGGING).encode(), 404, [("Content-Type", "text/html")]
    )
    missing = server.url("/missing")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, start) as page:
        page.evaluate("location.href = '/missing'; 0")
        assert _next_document(page).url == missing
        page.evaluate("window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        # It unloads, as a page that the cache does not keep does.
        assert page.evaluate("sessionStorage.hide") == "false,"
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=missing, status=404)
        assert page.evaluate("[window.marker ?? null, log]") == [None, ["pageshow:false"]]


def test_the_back_forward_cache_keeps_six_pages(session: mitto.Session, server: RecordingServer) -> None:
    urls = [server.page(f"/p{number}", f"<title>P{number}</title>") for number in range(8)]
    marker = "[location.pathname, window.marker ?? null]"

    with _open_page(session, urls[0]) as page:
        for number in range(1, 8):
            page.evaluate(f"window.marker = 1; location.href = '/p{number}'; 0")
            assert _next_document(page).url == urls[number]
        # The seventh page that it keeps makes it let the oldest go, the first page, which loads anew.
        page.evaluate("history.go(-7)")
        assert _next_document(page) == PageNavigation(url=urls[0], status=200)
        assert page.evaluate(marker) == ["/p0", None]
        # Keeping the last page let the second go, but the third comes back.
        page.evaluate("history.go(2)")
        assert _next_document(page) == PageNavigation(url=urls[2], status=200, restored=True)
        assert page.evaluate(marker) == ["/p2", 1]
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=urls[1], status=200)
        assert page.evaluate(marker) == ["/p1", None]


def test_a_script_that_would_run_in_a_kept_page_evicts_it(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "window.bc = new BroadcastChannel('x'); bc.onmessage = () => log.push('message');"
            " window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        # A message of its channel would run its listener, which a frozen page cannot, so the cache lets it go
        # with no event, as Chrome evicts a page for a BroadcastChannel message.
        page.evaluate("new BroadcastChannel('x').postMessage(1); new Promise(r => setTimeout(r, 500))")
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("[window.marker ?? null, sessionStorage.hide, log]") == [None, "true,", ["pageshow:false"]]


def test_a_kept_page_hears_of_its_request_once_it_comes_back(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    held = threading.Event()
    server.routes["/held"] = Route(b"late", headers=[("Content-Type", "text/plain")], hold=held)

    with _open_page(session, first) as page:
        page.evaluate(
            "fetch('/held').then(r => r.text()).then(t => log.push('fetched:' + t)); window.marker = 1;"
            " setTimeout(() => { location.href = '/b'; }, 200); 0"
        )
        assert _next_document(page).url == second
        # The response comes while the cache keeps the page, which hears of it once it shows again, as Chrome's
        # loaders hold what comes for a frozen page.
        assert server.wait_for_path("/held")
        held.set()
        _wait(page, 500)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("[window.marker, log]") == [1, [*_KEPT_AND_BACK, "fetched:late"]]


def test_a_freeze_listener_can_use_its_document_before_the_page_is_kept(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "document.cookie = 'k=1; path=/';"
            " document.addEventListener('freeze', () => log.push('cookie:' + document.cookie));"
            " window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("[window.marker, log]") == [
            1,
            [
                "pageshow:false",
                "pagehide:true",
                "visibilitychange:hidden",
                "freeze",
                "cookie:k=1",
                "resume",
                "visibilitychange:visible",
                "pageshow:true",
            ],
        ]


def test_a_restored_page_uses_its_storage_and_has_pagereveal_before_its_frames(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page(
        "/a",
        "<title>A</title>"
        + _LOGGING
        + """<script>
addEventListener('pagereveal', () => log.push('pagereveal'));
addEventListener('pageshow', e => {
  if (!e.persisted) return;
  requestAnimationFrame(() => log.push('raf'));
  caches.keys().then(keys => log.push('caches:' + keys.length));
});
</script>""",
    )
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate("log.length = 0; window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 300)
        # The page's cache storage is its own again, and pagereveal comes in its first frame, before the frame's
        # callbacks, as Chrome enqueues it once the page shows.
        assert page.evaluate("[window.marker, log.filter(entry => entry != 'caches:0'), log.includes('caches:0')]") == [
            1,
            [
                "pagehide:true",
                "visibilitychange:hidden",
                "freeze",
                "resume",
                "visibilitychange:visible",
                "pageshow:true",
                "pagereveal",
                "raf",
            ],
            True,
        ]


def test_a_signal_that_times_out_while_its_page_is_kept_aborts_once_it_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "AbortSignal.timeout(300).addEventListener('abort', () => log.push('signal'));"
            " window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        # Its time comes while the cache keeps the page, frozen.
        _wait(page, 900)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("[window.marker, log]") == [1, [*_KEPT_AND_BACK, "signal"]]


def test_a_keepalive_request_outlives_the_eviction_of_its_page(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    held = threading.Event()
    server.routes["/keep"] = Route(b"", 302, [("Location", "/kept-after")], hold=held)
    server.route("/kept-after", b"kept", "text/plain")

    with _open_page(session, first) as page:
        page.evaluate(
            "window.bc = new BroadcastChannel('x'); bc.onmessage = () => 0;"
            " fetch('/keep', {keepalive: true}).catch(() => 0); window.marker = 1;"
            " setTimeout(() => { location.href = '/b'; }, 200); 0"
        )
        assert _next_document(page).url == second
        # A message of the channel evicts the page, whose keepalive request goes on, as at an unload.
        page.evaluate("new BroadcastChannel('x').postMessage(1); new Promise(r => setTimeout(r, 400))")
        held.set()
        assert server.wait_for_path("/kept-after")
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("window.marker ?? null") is None


def test_a_traversal_lets_the_kept_pages_of_the_browsing_instance_it_goes_into_go(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/ns", "<title>NS</title>", _NO_STORE)
    second = server.page("/b", "<title>B</title>" + _LOGGING)
    third = server.page("/c", "<title>C</title>")
    server.route("/fetched", b"", "text/plain", _NO_STORE)

    with _open_page(session, first) as page:
        # The first page is none that the cache keeps, so the second shares its browsing instance, and is kept in
        # it, as the third page gets one of its own.
        page.evaluate("fetch('/fetched').then(r => r.text()).then(() => { location.href = '/b'; return 0; })")
        assert _next_document(page).url == second
        page.evaluate("window.marker = 1; location.href = '/c'; 0")
        assert _next_document(page).url == third
        # A traversal past the second page to the first puts a new document in the browsing instance that the
        # second shares, which goes, as Chrome evicts a page for kConflictingBrowsingInstance.
        page.evaluate("history.go(-2)")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        page.evaluate("history.forward()")
        assert _next_document(page) == PageNavigation(url=second, status=200)
        assert page.evaluate("[window.marker ?? null, log]") == [None, ["pageshow:false"]]


def test_a_restored_page_s_activation_comes_from_the_entry_of_the_page_it_left(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        # The entry that it came from is the very object that entries() gives for it.
        assert page.evaluate(
            "[navigation.activation.from === navigation.entries()[1], navigation.activation.from.index,"
            " navigation.entries().length]"
        ) == [True, 1, 2]


def test_a_restore_goes_on_though_a_listener_prunes_its_entry(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")
    state = (
        "[location.pathname + location.search, window.marker ?? null,"
        " navigation.entries().map(entry => new URL(entry.url).pathname + new URL(entry.url).search),"
        " navigation.currentEntry.index]"
    )
    push = (
        "navigation.addEventListener('navigate', event => {"
        " if (event.navigationType == 'traverse') history.pushState(null, '', '?pushed'); }); history.forward(); 0"
    )

    with _open_page(session, first) as page:
        page.evaluate("window.marker = 'A'; location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("window.marker = 'B'; history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        # The forward traversal's navigate listener makes an entry, which pruned the kept page's, and the page comes
        # back all the same.
        page.evaluate(push)
        assert _next_document(page) == PageNavigation(url=second, status=200, restored=True)
        assert page.evaluate("window.marker") == "B"
        # The entry it made stays before it, and its document comes back from the cache, as in Chrome.
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first + "?pushed", status=200, restored=True)
        assert page.evaluate(state) == ["/a?pushed", "A", ["/a", "/a?pushed", "/b"], 1]


def test_a_message_of_another_page_evicts_a_kept_page_whose_origin_the_current_page_left(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    tab = server.page("/tab", "<title>Tab</title>")
    other = server.page("/other", "<title>Other</title>").replace("127.0.0.1", "localhost")

    with _open_page(session, first) as page, _open_page(session, tab) as sender:
        page.evaluate(
            "window.bc = new BroadcastChannel('x'); bc.onmessage = () => log.push('message');"
            f" window.marker = 1; location.href = '{other}'; 0"
        )
        assert _next_document(page).url == other
        # The Page's current document is of another origin, but the kept page of the channel's is let go all the same.
        sender.evaluate("new BroadcastChannel('x').postMessage(1); new Promise(r => setTimeout(r, 400))")
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("window.marker ?? null") is None
