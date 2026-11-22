"""Tests of a Page's session history within a document: pushState() and replaceState(), state, length and
scrollRestoration, back(), forward() and go(), and the Navigation API's entries and traversals, as Chrome 157 gives
them. Each expected value is what Chrome for Testing 157.0.8103.79 gave for the same script in a new tab.

A local server serves each page. Each test goes through both APIs, and needs a native library with the Page program
beside it, as CI's native job installs it on every Host.
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page, PageNavigation
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, without_favicon

pytestmark = pytest.mark.usefixtures("both_apis")


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


def _next(page: Page) -> PageNavigation:
    return page.wait_for_navigation(timeout=TIMEOUT_SECONDS)


def test_push_state_and_replace_state_change_the_entry_at_once(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>History</title>")
    script = """(() => {
      const r = [history.length, history.state];
      history.pushState(1, '', '?a');
      r.push(location.search, history.state, history.length);
      const pushed = history.state;
      r.push(history.state === pushed);
      history.replaceState({x: 2}, '', '#b');
      r.push(location.href.endsWith('?a#b'), JSON.stringify(history.state), history.length,
             history.state === history.state);
      history.replaceState(null, '', '');
      r.push(location.href.endsWith('?a#b'), history.state);
      history.pushState(null, '', ' ');
      r.push(location.hash, location.search);
      return r;
    })()"""

    with _open_page(session, url) as page:
        # The URL, the state and the length change before each call returns, and history.state gives the same
        # object until the state changes. A URL of white space is the document's without its fragment.
        assert page.evaluate(script) == [1, None, "?a", 1, 2, True, True, '{"x":2}', 2, True, True, None, "", "?a"]
        # The Session hears of each new URL, as a navigation within the document.
        assert _next(page) == PageNavigation(url=url + "?a", status=0, same_document=True)
        assert _next(page) == PageNavigation(url=url + "?a#b", status=0, same_document=True)
        assert _next(page) == PageNavigation(url=url + "?a#b", status=0, same_document=True)
        assert _next(page) == PageNavigation(url=url + "?a", status=0, same_document=True)
        assert page.url == url + "?a"
    assert without_favicon(server.paths()) == ["/"]


def test_push_state_refuses_what_chrome_refuses(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/errors", "<title>Errors</title>")
    script = """(() => {
      const r = [];
      for (const f of [
        () => history.pushState(1, '', 'http://www.example.com/test.html'),
        () => history.replaceState(() => 1, ''),
        () => history.pushState(1, '', 'http://['),
        () => history.pushState(1),
        () => navigation.updateCurrentEntry({state: () => 1}),
      ]) {
        try { f(); r.push('none'); } catch (e) { r.push(e.name + ': ' + e.message); }
      }
      return [r, history.length];
    })()"""

    with _open_page(session, url) as page:
        assert page.evaluate(script) == [
            [
                "SecurityError: Failed to execute 'pushState' on 'History': A history state object with URL "
                f"'http://www.example.com/test.html' cannot be created in a document with origin '{server.origin}' "
                f"and URL '{url}'.",
                "DataCloneError: Failed to execute 'replaceState' on 'History': () => 1 could not be cloned.",
                "SecurityError: Failed to execute 'pushState' on 'History': A history state object with URL "
                f"'http://[/' cannot be created in a document with origin '{server.origin}' and URL '{url}'.",
                "TypeError: Failed to execute 'pushState' on 'History': 2 arguments required, but only 1 present.",
                "DataCloneError: Failed to execute 'updateCurrentEntry' on 'Navigation': () => 1 could not be cloned.",
            ],
            1,
        ]


def test_back_traverses_within_the_document_from_a_later_task(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Traverse</title>")
    script = """new Promise(resolve => {
      const seen = [];
      navigation.addEventListener('navigate', e => seen.push(`navigate ${e.navigationType} ${e.cancelable} `
        + `${e.canIntercept} ${e.hashChange} ${e.destination.sameDocument} ${e.destination.index} `
        + `${JSON.stringify(e.destination.getState())}`));
      navigation.addEventListener('currententrychange', e =>
        seen.push(`currententrychange ${e.from.index} ${navigation.currentEntry.index}`));
      navigation.addEventListener('navigatesuccess', () => seen.push('navigatesuccess'));
      addEventListener('popstate', e => seen.push(`popstate ${JSON.stringify(e.state)} ${e.state === history.state} `
        + `${location.hash} ${e.bubbles} ${e.cancelable}`));
      addEventListener('hashchange', e => {
        seen.push(`hashchange ${new URL(e.oldURL).hash} ${new URL(e.newURL).hash}`);
        setTimeout(() => resolve(seen), 50);
      });
      history.pushState({s: 1}, '', '#one');
      history.pushState({s: 2}, '', '#two');
      history.back();
      seen.push(`after back ${location.hash} ${history.length} ${JSON.stringify(history.state)}`);
    })"""

    with _open_page(session, url) as page:
        # Nothing moves until a later task, which runs the navigate event, the commit with its entry's state, then
        # popstate with the object that history.state gives, and hashchange from a task after it.
        assert page.evaluate(script) == [
            "navigate push true true false true -1 undefined",
            "currententrychange 0 1",
            "navigate push true true false true -1 undefined",
            "currententrychange 1 2",
            'after back #two 3 {"s":2}',
            "navigatesuccess",
            "navigate traverse true true true true 1 undefined",
            "currententrychange 2 1",
            "navigatesuccess",
            'popstate {"s":1} true #one false false',
            "hashchange #two #one",
        ]
        assert page.evaluate("[history.length, navigation.currentEntry.index, navigation.canGoForward]") == [
            3,
            1,
            True,
        ]


def test_each_go_goes_from_where_the_one_before_it_went(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Go</title>")
    script = """new Promise(resolve => {
      location.hash = 'foo';
      location.hash = 'bar';
      location.hash = 'baz';
      const seen = [];
      let count = 0;
      addEventListener('hashchange', e => {
        count += 1;
        if (count <= 3) return;
        seen.push(new URL(e.newURL).hash);
        if (count == 5) setTimeout(() => resolve([seen, location.hash, history.length]), 50);
      });
      setTimeout(() => {
        history.go(-2);
        seen.push('now ' + location.hash);
        history.go(-1);
      }, 0);
    })"""

    with _open_page(session, url) as page:
        assert page.evaluate(script) == [["now #baz", "#foo", ""], "", 4]
        # Past the ends, go() does nothing.
        assert (
            page.evaluate("history.go(-1); history.go(10); new Promise(r => setTimeout(() => r(location.hash), 50))")
            == ""
        )


def test_a_traversal_goes_to_the_entry_it_asked_for_when_it_asked(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<title>Asked</title>")
    script = """new Promise(resolve => {
      location.hash = '#1';
      location.hash = '#2';
      const anchor = document.createElement('a');
      anchor.href = '#3';
      anchor.onclick = () => history.back();
      const seen = [];
      onpopstate = () => {
        seen.push(location.hash);
        if (seen.length == 2) resolve([seen, location.hash, history.length]);
      };
      anchor.click();
    })"""

    with _open_page(session, url) as page:
        # back() asks for the entry before #2, and the link's navigation to #3, which commits before the traversal
        # runs, does not change which.
        assert page.evaluate(script) == [["#3", "#1"], "#1", 4]


def test_go_with_no_offset_reloads(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Reload</title>")

    with _open_page(session, url) as page:
        page.evaluate("history.go()")
        assert _next(page) == PageNavigation(url=url, status=200)
    # A reload validates what the cache holds.
    first, reloaded = server.headers_of("/")
    assert "Cache-Control" not in dict(first)
    assert dict(reloaded)["Cache-Control"] == "max-age=0"


def test_the_session_history_keeps_50_entries(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Cap</title>")
    script = """new Promise(resolve => {
      for (let i = 0; i < 60; i++) history.pushState(i, '', '?' + i);
      const r = [history.length, navigation.entries().length, navigation.currentEntry.index];
      setTimeout(() => {
        r.push(history.length, navigation.entries().length, navigation.currentEntry.index,
               navigation.entries()[0].url.split('?')[1]);
        resolve(r);
      }, 300);
    })"""

    with _open_page(session, url) as page:
        # history.length counts 50 at once, and the Navigation API drops the oldest entries once the session history
        # tells it, from a later task.
        assert page.evaluate(script) == [50, 61, 60, 50, 50, 49, "10"]


def test_scroll_restoration_belongs_to_each_entry(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Scroll</title>")
    script = """new Promise(resolve => {
      const r = [history.scrollRestoration];
      history.scrollRestoration = 'manual';
      history.pushState(1, '', '#1');
      r.push(history.scrollRestoration);
      history.scrollRestoration = 'auto';
      history.pushState(2, '', '#2');
      r.push(history.scrollRestoration);
      history.scrollRestoration = 'nonsense';
      r.push(history.scrollRestoration);
      history.scrollRestoration = 'manual';
      addEventListener('popstate', () => { r.push(history.scrollRestoration); history.forward(); }, {once: true});
      addEventListener('hashchange', () => {
        addEventListener('popstate', () => { r.push(history.scrollRestoration); resolve(r); }, {once: true});
      }, {once: true});
      history.back();
    })"""

    with _open_page(session, url) as page:
        # A new entry within the document takes the one before it's, a value that is none of the enumeration's does
        # nothing but warn, as Blink's bindings do, and a traversal reads its entry's own.
        assert page.evaluate(script) == ["auto", "manual", "auto", "auto", "auto", "manual"]
        assert page.console_messages == [
            _native.ConsoleMessage(
                level="warning",
                text="The provided value 'nonsense' is not a valid enum value of type ScrollRestoration.",
            )
        ]


def test_the_navigation_api_traverses_within_the_document(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Navigation</title>")
    script = """(async () => {
      const r = [];
      history.pushState(null, '', '#a');
      history.pushState(null, '', '#b');
      navigation.addEventListener('navigate', e => r.push('navigate ' + e.navigationType + ' ' + e.info),
                                  {once: true});
      const res = navigation.back({info: 'x'});
      r.push('sync ' + location.hash);
      const c = await res.committed;
      r.push('committed ' + c.url.split('#')[1] + ' ' + c.index);
      const f = await res.finished;
      r.push('finished ' + (f === c));
      try { await navigation.traverseTo('nope').finished; } catch (e) { r.push(e.name + ': ' + e.message); }
      const cur = await navigation.traverseTo(navigation.currentEntry.key).finished;
      r.push('same ' + (cur === navigation.currentEntry));
      navigation.addEventListener('currententrychange',
        e => r.push('cec ' + (e.from === navigation.currentEntry) + ' ' + e.navigationType), {once: true});
      navigation.updateCurrentEntry({state: {n: 1}});
      r.push(JSON.stringify(navigation.currentEntry.getState()));
      const two = navigation.forward();
      const again = navigation.forward();
      r.push('same result ' + (two.finished === again.finished));
      await two.finished;
      r.push('forward ' + location.hash + ' ' + JSON.stringify(navigation.entries()[1].getState()));
      try { await navigation.forward().finished; } catch (e) { r.push(e.name + ': ' + e.message); }
      return r;
    })()"""

    with _open_page(session, url) as page:
        assert page.evaluate(script) == [
            "sync #b",
            "navigate traverse x",
            "committed a 1",
            "finished true",
            "InvalidStateError: Invalid key",
            "same true",
            "cec true null",
            '{"n":1}',
            "same result true",
            'forward #b {"n":1}',
            "InvalidStateError: Cannot go forward",
        ]


def test_canceling_the_navigate_event_of_a_traversal_keeps_the_entry(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<title>Cancel</title>")
    script = """new Promise(resolve => {
      const r = [];
      history.pushState(null, '', '#a');
      navigation.addEventListener('navigate', e => {
        r.push('navigate ' + e.navigationType + ' ' + e.cancelable);
        e.preventDefault();
      }, {once: true});
      navigation.addEventListener('navigateerror', e => r.push('navigateerror ' + e.error.name));
      addEventListener('popstate', () => r.push('popstate'));
      navigation.back().finished.catch(e => r.push('finished ' + e.name + ': ' + e.message));
      setTimeout(() => { r.push(location.hash, navigation.currentEntry.index, history.length); resolve(r); }, 200);
    })"""

    with _open_page(session, url) as page:
        assert page.evaluate(script) == [
            "navigate traverse true",
            "navigateerror AbortError",
            "finished AbortError: Navigation was aborted",
            "#a",
            1,
            2,
        ]


def test_a_new_document_adds_an_entry_that_history_counts(session: mitto.Session, server: RecordingServer) -> None:
    second = server.page("/second", "<title>Second</title>")
    first = server.page("/first", "<title>First</title>")
    entries = (
        "[history.length, navigation.entries().map(e => new URL(e.url).pathname), navigation.activation.navigationType,"
        " navigation.activation.from && new URL(navigation.activation.from.url).pathname]"
    )

    with _open_page(session, first) as page:
        # The first document takes the place of the tab's initial entry.
        assert page.evaluate(entries) == [1, ["/first"], "replace", None]
        page.evaluate("location.href = '/second'; 0")
        assert _next(page).url == second
        assert page.evaluate(entries) == [2, ["/first", "/second"], "push", "/first"]
        page.evaluate("location.replace('/first'); 0")
        assert _next(page).url == first
        assert page.evaluate(entries) == [2, ["/first", "/first"], "replace", "/second"]
