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
        # The page's own first pagereveal comes with its first frame, which may come after the page has opened.
        page.evaluate(
            "new Promise(resolve => {"
            " if (log.includes('pagereveal')) { resolve(0); }"
            " else { addEventListener('pagereveal', () => resolve(0), {once: true}); } })",
            timeout=TIMEOUT_SECONDS,
        )
        page.evaluate("log.length = 0; window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        # What the page's frame and its cache storage say of themselves, whenever they do.
        page.evaluate(
            "new Promise(resolve => { const look = () =>"
            " log.includes('raf') && log.some(entry => entry.startsWith('caches:'))"
            " ? resolve(0) : setTimeout(look, 10); look(); })",
            timeout=TIMEOUT_SECONDS,
        )
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


def test_a_scheduled_task_of_a_kept_page_runs_once_it_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "scheduler.postTask(() => log.push('task'), {delay: 300}); window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 900)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("[window.marker, log]") == [1, [*_KEPT_AND_BACK, "task"]]


@pytest.mark.parametrize(
    ("change", "expected"),
    [
        ("pushState", ["/a?ph", 3, ["/a", "/a?ph", "/b"]]),
        ("replaceState", ["/a?ph", 2, ["/a?ph", "/b"]]),
    ],
)
def test_what_a_pagehide_listener_of_a_kept_page_does_to_the_history_stays(
    session: mitto.Session, server: RecordingServer, change: str, expected: list[Any]
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            f"addEventListener('pagehide', () => history.{change}({{x: 1}}, '', '?ph'));"
            " window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first + "?ph", status=200, restored=True)
        # The listener's entry is the one that the page is kept at, with its state.
        assert page.evaluate(
            "[window.marker, location.pathname + location.search, history.length,"
            " navigation.entries().map(entry => new URL(entry.url).pathname + new URL(entry.url).search),"
            " history.state]"
        ) == [1, *expected, {"x": 1}]


@pytest.mark.parametrize(
    ("consume", "restored"),
    [
        # A preload that no script's fetch takes.
        (False, True),
        # One that a fetch takes, whose response a script then has.
        (True, False),
    ],
    ids=["unused", "consumed"],
)
def test_a_preload_keeps_a_no_store_page_out_only_once_a_fetch_takes_it(
    session: mitto.Session, server: RecordingServer, consume: bool, restored: bool
) -> None:
    first = server.page("/ns", "<title>NS</title>" + _LOGGING, _NO_STORE)
    second = server.page("/b", "<title>B</title>")
    server.route("/preloaded", b"", "text/plain", _NO_STORE)
    take = "fetch('/preloaded').then(r => r.text())" if consume else "Promise.resolve()"

    with _open_page(session, first) as page:
        page.evaluate(
            "const link = document.createElement('link'); link.rel = 'preload'; link.as = 'fetch';"
            " link.href = '/preloaded'; link.crossOrigin = 'anonymous'; document.head.append(link);"
            f" new Promise(r => setTimeout(r, 300)).then(() => {take}).then(() => {{"
            " window.marker = 1; location.href = '/b'; return 0; })"
        )
        assert _next_document(page).url == second
        assert page.evaluate("sessionStorage.hide") == ("true," if restored else "false,")
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=restored)
        assert page.evaluate("window.marker ?? null") == (1 if restored else None)


def test_scheduled_tasks_of_a_kept_page_run_by_priority_once_it_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "addEventListener('pagehide', () => {"
            # The background task is due first, while the cache keeps the page.
            " scheduler.postTask(() => log.push('background'), {priority: 'background', delay: 10});"
            " scheduler.postTask(() => log.push('user-blocking'), {priority: 'user-blocking', delay: 300});"
            " const controller = new AbortController();"
            " scheduler.postTask(() => log.push('aborted'), {signal: controller.signal}).catch(() => {});"
            " controller.abort(); }); window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 900)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("[window.marker, log]") == [1, [*_KEPT_AND_BACK, "user-blocking", "background"]]


def test_what_a_freeze_listener_asks_is_answered_once_its_page_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    ask = (
        "addEventListener('unhandledrejection', event => { log.push('unhandled'); event.preventDefault(); });"
        "document.addEventListener('freeze', () => {"
        " cookieStore.getAll().then(() => log.push('getAll'));"
        " cookieStore.set('x', '1').then(() => log.push('set'));"
        " navigator.locks.query().then(() => log.push('query'));"
        " navigator.permissions.query({name: 'geolocation'}).then(() => log.push('permission'));"
        " chrome.app.installState(() => log.push('installState'));"
        " Promise.reject('no'); });"
    )

    with _open_page(session, first) as page:
        # An answer that came before leaves nothing that keeps the page out.
        page.evaluate("navigator.storage.estimate().then(() => 0)")
        page.evaluate(f"{ask} window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        _wait(page, 400)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 400)
        marker, log = page.evaluate("[window.marker, log]")
        assert marker == 1
        assert log[: len(_KEPT_AND_BACK)] == _KEPT_AND_BACK
        # The answers come in the order in which the browser gives them, which differs by the API.
        assert sorted(log[len(_KEPT_AND_BACK) :]) == [
            "getAll",
            "installState",
            "permission",
            "query",
            "set",
            "unhandled",
        ]


def test_a_restored_page_has_its_window_name_again(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>").replace("127.0.0.1", "localhost")

    with _open_page(session, first) as page:
        page.evaluate(
            "window.name = 'A'; addEventListener('pageshow', () => { window.shown = name; });"
            f" location.href = '{second}'; 0"
        )
        assert _next_document(page).url == second
        page.evaluate("window.name = 'B'; history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("[window.shown, window.name]") == ["A", "A"]


def test_a_page_kept_at_an_entry_that_an_earlier_document_made_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    within = server.url("/a2")
    second = server.page("/b", "<title>B</title>")
    third = server.page("/c", "<title>C</title>")

    with _open_page(session, first) as page:
        page.evaluate("history.pushState(1, '', '/a2'); location.href = '/b'; 0")
        assert _next_document(page).url == second
        # A new document takes the first entry, as the cache kept the one before it at the second, and goes there.
        page.evaluate("history.go(-2)")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        page.evaluate("window.marker = 3; history.forward(); new Promise(r => { onpopstate = r; }).then(() => 0)")
        assert page.evaluate("[location.pathname, window.marker]") == ["/a2", 3]
        page.evaluate("location.href = '/c'; 0")
        assert _next_document(page).url == third
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=within, status=200, restored=True)
        assert page.evaluate("[location.pathname, window.marker, history.state, log]") == [
            "/a2",
            3,
            1,
            _KEPT_AND_BACK,
        ]


_ENTRIES = (
    "[location.pathname + location.search + location.hash, window.marker ?? null, history.length,"
    " navigation.entries().map(entry => entry.url.slice(new URL(entry.url).origin.length)),"
    " navigation.currentEntry.index]"
)


@pytest.mark.parametrize(
    ("ask", "kept"),
    [
        ("history.pushState(1, '', '?asked')", False),
        ("history.replaceState(1, '', '?asked')", False),
        ("location.hash = 'asked'", False),
        ("location.href = '/asked'", False),
        ("navigation.updateCurrentEntry({state: 5})", True),
        # Nothing lies before its entry, so the browser hears nothing of it.
        ("history.back()", True),
    ],
)
def test_what_a_freeze_listener_asks_of_the_history_lets_its_page_go(
    session: mitto.Session, server: RecordingServer, ask: str, kept: bool
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            f"document.addEventListener('freeze', () => {{ {ask}; }}); window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        # Chrome's browser has the page in its cache as it freezes, ignores what it asks, and evicts it.
        assert page.evaluate(_ENTRIES) == ["/b", None, 2, ["/a", "/b"], 1]
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=kept)
        assert page.evaluate(_ENTRIES) == ["/a", 1 if kept else None, 2, ["/a", "/b"], 0]


@pytest.mark.parametrize(
    ("ask", "kept"),
    [
        ("history.pushState(1, '', '?asked')", False),
        ("history.replaceState(1, '', '?asked')", False),
        ("location.hash = 'asked'", False),
        ("location.href = '/asked'", False),
        ("history.back()", False),
        ("navigation.updateCurrentEntry({state: 5})", True),
    ],
)
def test_what_a_page_asks_of_the_history_as_a_restore_takes_its_place_lets_it_go(
    session: mitto.Session, server: RecordingServer, ask: str, kept: bool
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate("window.marker = 'A'; location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate(f"window.marker = 'B'; addEventListener('pagehide', () => {{ {ask}; }}); history.back(); 0")
        # Chrome's browser has committed the restore already, and the page that leaves is in its cache.
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate(_ENTRIES) == ["/a", "A", 2, ["/a", "/b"], 0]
        page.evaluate("history.forward()")
        assert _next_document(page) == PageNavigation(url=second, status=200, restored=kept)
        assert page.evaluate(_ENTRIES) == ["/b", "B" if kept else None, 2, ["/a", "/b"], 1]


def test_a_fragment_that_a_pagehide_listener_of_a_kept_page_goes_to_stays(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "addEventListener('pagehide', () => { location.hash = 'x'; }); window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first + "#x", status=200, restored=True)
        assert page.evaluate(_ENTRIES) == ["/a#x", 1, 3, ["/a", "/a#x", "/b"], 1]


def test_a_freeze_listener_sends_keepalive_requests_only(session: mitto.Session, server: RecordingServer) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    server.route("/kept", b"k", "text/plain")
    ask = (
        "document.addEventListener('freeze', () => {"
        " fetch('/fetched').then(() => log.push('fetched'), error => log.push(error.message));"
        " fetch('data:text/plain,x').then(() => log.push('data'), error => log.push('data: ' + error.name));"
        " const script = document.createElement('script'); script.onerror = () => log.push('script');"
        " script.src = '/script'; document.head.append(script);"
        " fetch('/kept', {keepalive: true}).then(() => log.push('kept')); });"
    )

    with _open_page(session, first) as page:
        page.evaluate(f"{ask} window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        assert server.wait_for_path("/kept")
        _wait(page, 200)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 200)
        marker, log = page.evaluate("[window.marker, log]")
        assert marker == 1
        assert log[: len(_KEPT_AND_BACK)] == _KEPT_AND_BACK
        assert sorted(log[len(_KEPT_AND_BACK) :]) == ["Failed to fetch", "data: TypeError", "kept", "script"]
        refused = [message.text for message in page.console_messages if "onfreeze" in message.text]
        assert refused == [
            f"Only fetch keepalive is allowed during onfreeze: {url}"
            for url in (server.url("/fetched"), "data:text/plain,x", server.url("/script"))
        ]
    assert without_favicon(server.paths()) == ["/a", "/b", "/kept"]


def test_the_performance_timeline_has_the_visibility_of_a_kept_page(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    names = "performance.getEntriesByType('visibility-state').map(entry => entry.name).join()"

    with _open_page(session, first) as page:
        page.evaluate(
            f"window.states = []; addEventListener('pagehide', () => states.push({names}));"
            f" document.addEventListener('visibilitychange', () => states.push({names})); location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate(
            "[states, performance.getEntriesByType('visibility-state').map(entry => [entry.startTime > 0,"
            " entry.duration])]"
        ) == [["visible", "visible,hidden", "visible,hidden,visible"], [[False, 0], [True, 0], [True, 0]]]


def test_a_page_whose_scripts_never_read_its_timeline_has_its_visibility_too(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate(
            "performance.getEntriesByType('visibility-state').map(entry => [entry.name, entry.startTime > 0])"
        ) == [["visible", False], ["hidden", True], ["visible", True]]


def test_a_watch_that_a_resume_listener_clears_hears_no_timeout_that_waited(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        # Headless Chrome denies the permission before any timeout, so the values here are those of Blink's
        # Geolocation::clearWatch().
        page.evaluate(
            "const watch = () => navigator.geolocation.watchPosition(() => {}, () => log.push('timeout'),"
            " {timeout: 300}); const cleared = watch(); watch();"
            " document.addEventListener('resume', () => navigator.geolocation.clearWatch(cleared));"
            " location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 600)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("log") == [*_KEPT_AND_BACK, "timeout"]


def test_the_first_of_a_kept_page_s_responses_can_cancel_the_next(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    held = threading.Event()
    for path in ("/1", "/2"):
        server.routes[path] = Route(b"x", headers=[("Content-Type", "text/plain")], hold=held)

    with _open_page(session, first) as page:
        page.evaluate(
            "const controller = new AbortController();"
            " fetch('/1').then(() => { log.push('first'); controller.abort(); });"
            " fetch('/2', {signal: controller.signal}).then(() => log.push('second'), error => log.push(error.name));"
            " window.marker = 1; setTimeout(() => { location.href = '/b'; }, 200); 0"
        )
        assert _next_document(page).url == second
        assert server.wait_for_path("/1")
        assert server.wait_for_path("/2")
        held.set()
        _wait(page, 500)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 200)
        # Chrome's loaders go in no order, so it may be the other way round there, but each flushes its response in a
        # task of its own.
        assert page.evaluate("[window.marker, log]") == [1, [*_KEPT_AND_BACK, "first", "AbortError"]]


@pytest.mark.parametrize(
    ("size", "kept"),
    [
        # More than the Session sends without credit.
        (300_000, True),
        # More than BackForwardCacheBufferLimitTracker lets a process buffer.
        (1_100_000, False),
    ],
)
def test_a_body_that_comes_while_its_page_is_kept_waits_up_to_a_limit(
    session: mitto.Session, server: RecordingServer, size: int, kept: bool
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")
    held = threading.Event()
    server.routes["/big"] = Route(b"x" * size, headers=[("Content-Type", "text/plain")], hold=held)

    with _open_page(session, first) as page:
        page.evaluate(
            "fetch('/big').then(r => r.text()).then(text => log.push(text.length)); window.marker = 1;"
            " setTimeout(() => { location.href = '/b'; }, 200); 0"
        )
        assert _next_document(page).url == second
        assert server.wait_for_path("/big")
        held.set()
        _wait(page, 1000)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=kept)
        _wait(page, 300)
        assert page.evaluate("[window.marker ?? null, log]") == (
            [1, [*_KEPT_AND_BACK, size]] if kept else [None, ["pageshow:false"]]
        )


@pytest.mark.parametrize(
    ("ask", "persisted", "kept"),
    [
        # A request that its signal refuses reaches no service.
        ("navigator.credentials.get({otp: {transport: ['sms']}, signal: AbortSignal.abort()})", True, True),
        # WebOTP and the password store keep a page out for good.
        ("navigator.credentials.get({otp: {transport: ['sms']}})", False, False),
        ("navigator.credentials.get({password: true})", False, False),
        # A WebAuthn request keeps it out while the browser has it, as it asks once the page has frozen.
        ("navigator.credentials.get({publicKey: {challenge: new Uint8Array(16)}})", True, False),
        (
            "{ const controller = new AbortController(); addEventListener('pagehide', () => controller.abort());"
            " navigator.credentials.get({signal: controller.signal, publicKey: {challenge: new Uint8Array(16)}})"
            ".catch(() => {}); } Promise.resolve()",
            True,
            True,
        ),
    ],
    ids=["aborted-otp", "otp", "password", "public-key", "aborted-public-key"],
)
def test_a_credential_request_keeps_a_page_out(
    session: mitto.Session, server: RecordingServer, ask: str, persisted: bool, kept: bool
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING).replace("127.0.0.1", "localhost")
    second = server.page("/b", "<title>B</title>").replace("127.0.0.1", "localhost")

    with _open_page(session, first) as page:
        page.evaluate(f"{ask}.catch(() => {{}}); new Promise(r => setTimeout(r, 100)).then(() => 0)")
        page.evaluate("window.marker = 1; location.href = '/b'; 0")
        assert _next_document(page).url == second
        assert page.evaluate("sessionStorage.hide") == ("true," if persisted else "false,")
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=kept)
        assert page.evaluate("window.marker ?? null") == (1 if kept else None)


_CELL = "const cell = new Int32Array(new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer);"


def test_a_promise_that_v8_settles_while_its_page_is_kept_has_its_reactions_as_it_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>" + _LOGGING)
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        # V8 settles the promise itself once the wait's time is up, on the page's own thread.
        page.evaluate(
            f"{_CELL} document.addEventListener('freeze', () => {{"
            " Atomics.waitAsync(cell, 0, 0, 100).value.then(value => { log.push(value); localStorage.settled = 'yes'; });"
            " }); document.addEventListener('resume', () => Promise.resolve().then(() => log.push('resumed')));"
            " localStorage.clear(); window.marker = 1; location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 500)
        # The page's agent runs no microtask while it is frozen.
        assert page.evaluate("localStorage.settled ?? null") is None
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("[window.marker, log]") == [
            1,
            [
                "pageshow:false",
                "pagehide:true",
                "visibilitychange:hidden",
                "freeze",
                "timed-out",
                "resume",
                "resumed",
                "visibilitychange:visible",
                "pageshow:true",
            ],
        ]
        # Its microtasks go on as before.
        assert page.evaluate("Promise.resolve(1).then(x => x + 1)") == 2
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == second
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("Promise.resolve(log.at(-1)).then(x => x)") == "pageshow:true"


def test_the_reactions_of_a_kept_page_run_in_order_past_what_they_queue(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title><script>window.log = [];</script>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            f"{_CELL} addEventListener('x', () => {{ Promise.resolve().then(() => log.push('inner')); }});"
            " document.addEventListener('freeze', () => {"
            " Atomics.waitAsync(cell, 0, 0, 100).value.then(() => {"
            " log.push('first'); dispatchEvent(new Event('x')); log.push('after the event'); });"
            " Atomics.waitAsync(cell, 0, 0, 120).value.then(() => log.push('second')); });"
            " location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 500)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        assert page.evaluate("log") == ["first", "after the event", "second", "inner"]


def test_a_page_that_goes_takes_the_reactions_that_waited_with_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            f"{_CELL} document.addEventListener('freeze', () => {{"
            " Atomics.waitAsync(cell, 0, 0, 50).value.then(() => { localStorage.settled = 'yes'; }); });"
            " localStorage.clear(); window.bc = new BroadcastChannel('x'); bc.onmessage = () => {};"
            " location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 300)
        # The message evicts the page, and its reactions never run.
        page.evaluate("new BroadcastChannel('x').postMessage(1); new Promise(r => setTimeout(r, 300))")
        assert page.evaluate("localStorage.settled ?? null") is None
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200)
        assert page.evaluate("localStorage.settled ?? null") is None


@pytest.mark.parametrize(
    ("before", "after", "expected"),
    [
        # What waited in a queue comes before what a pageshow listener posts, and what was delayed after it.
        ("scheduler.postTask(() => log.push('old'))", "scheduler.postTask(() => log.push('new'))", ["old", "new"]),
        (
            "scheduler.yield().then(() => log.push('old'))",
            "scheduler.yield().then(() => log.push('new'))",
            ["old", "new"],
        ),
        ("setTimeout(() => log.push('old'), 0)", "setTimeout(() => log.push('new'), 0)", ["old", "new"]),
        ("channel.port2.postMessage('old')", "channel.port2.postMessage('new')", ["old", "new"]),
        (
            "AbortSignal.timeout(0).onabort = () => log.push('old')",
            "setTimeout(() => log.push('new'), 0)",
            ["old", "new"],
        ),
        (
            "AbortSignal.timeout(50).onabort = () => log.push('old')",
            "setTimeout(() => log.push('new'), 0)",
            ["new", "old"],
        ),
        (
            "scheduler.postTask(() => log.push('old'), {delay: 100})",
            "scheduler.postTask(() => log.push('new'))",
            ["new", "old"],
        ),
        ("setTimeout(() => log.push('old'), 100)", "setTimeout(() => log.push('new'), 0)", ["new", "old"]),
        (
            "requestIdleCallback(deadline => log.push('old:' + deadline.didTimeout), {timeout: 50})",
            "setTimeout(() => log.push('new'), 0)",
            ["new", "old:true"],
        ),
    ],
    ids=["task", "yield", "timeout", "message", "signal", "delayed-signal", "delayed-task", "delayed-timeout", "idle"],
)
def test_what_waited_comes_before_what_a_pageshow_listener_posts_but_for_what_was_delayed(
    session: mitto.Session, server: RecordingServer, before: str, after: str, expected: list[str]
) -> None:
    first = server.page("/a", "<title>A</title><script>window.log = [];</script>")
    second = server.page("/b", "<title>B</title>")

    with _open_page(session, first) as page:
        page.evaluate(
            "const channel = new MessageChannel(); channel.port1.onmessage = event => log.push(event.data);"
            f" document.addEventListener('freeze', () => {{ {before}; }});"
            f" addEventListener('pageshow', event => {{ if (event.persisted) {{ {after}; }} }});"
            " location.href = '/b'; 0"
        )
        assert _next_document(page).url == second
        _wait(page, 400)
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=first, status=200, restored=True)
        _wait(page, 100)
        assert page.evaluate("log") == expected


def test_a_page_has_opened_once_a_document_that_had_not_loaded_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    held = threading.Event()
    server.routes["/held"] = Route(b"x", headers=[("Content-Type", "text/plain")], hold=held)
    first = server.page(
        "/a",
        "<title>A</title>" + _LOGGING + "<script>"
        "addEventListener('load', () => log.push('load'));"
        "document.addEventListener('DOMContentLoaded', () => {"
        "  setTimeout(() => { window.marker = 1; navigation.navigate('/b', {history: 'push'}); }, 50);"
        "});"
        "</script><img src='/held'>",
    )
    server.page("/b", "<title>B</title><script>setTimeout(() => history.back(), 0)</script><img src='/held'>")

    try:
        # No document loads: the first stopped as it left, and the second as the cache gave the first its place again.
        with _open_page(session, first) as page:
            assert page.evaluate("[location.pathname, window.marker, log.at(-1), document.readyState]") == [
                "/a",
                1,
                "pageshow:true",
                "complete",
            ]
            held.set()
            _wait(page, 200)
            assert page.evaluate("log.includes('load')") is False
    finally:
        held.set()


def test_a_kept_page_whose_pagehide_listener_traversed_can_traverse_once_it_comes_back(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/a", "<title>A</title>")
    second = server.page("/b", "<title>B</title><script>window.log = [];</script>")
    third = server.page("/c", "<title>C</title>")

    with _open_page(session, first) as page:
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == second
        # A Known Gap: the traversal of the listener goes on in Chrome, to the first page.
        page.evaluate(
            "addEventListener('pagehide', () => {"
            "  window.first = navigation.back().finished;"
            "  first.then(() => log.push('finished'), error => log.push(error.name));"
            "}, {once: true});"
            "addEventListener('pageshow', () => log.push(navigation.back().finished === first), {once: true});"
            " location.href = '/c'; 0"
        )
        assert _next_document(page).url == third
        page.evaluate("history.back()")
        assert _next_document(page) == PageNavigation(url=second, status=200, restored=True)
        assert _next_document(page).url == first
        page.evaluate("history.forward()")
        assert _next_document(page) == PageNavigation(url=second, status=200, restored=True)
        # The promises of the one before it never settle.
        _wait(page, 100)
        assert page.evaluate("log") == [False]
