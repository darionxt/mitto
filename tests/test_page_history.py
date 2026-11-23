"""Tests of a Page's session history within a document: pushState() and replaceState(), state, length and
scrollRestoration, back(), forward() and go(), and the Navigation API's entries and traversals, as Chrome 157 gives
them. Each expected value is what Chrome for Testing 157.0.8103.79 gave for the same script in a new tab.

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
from tests.page_server import HTML, TIMEOUT_SECONDS, RecordingServer, Route, without_favicon

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


def _next_document(page: Page) -> PageNavigation:
    """Return the next navigation to another document, past those within the document before it."""
    while (navigation := _next(page)).same_document:
        pass
    return navigation


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


def test_a_request_after_push_state_names_the_new_url(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/a", "<title>Referer</title>")
    server.route("/one", b"1", "text/plain")
    server.route("/two", b"2", "text/plain")
    script = """(async () => {
      history.pushState(null, '', '/pushed?x#f');
      await fetch('/one');
      history.replaceState(null, '', '/replaced');
      await fetch('/two');
      return 0;
    })()"""

    with _open_page(session, url) as page:
        # The Session hears each URL that the history API gives the document, which its requests then name.
        page.evaluate(script)
        referers = [
            {name.lower(): value for name, value in headers}.get("referer")
            for path in ("/one", "/two")
            for headers in server.headers_of(path)
        ]
        assert referers == [server.url("/pushed?x"), server.url("/replaced")]


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


def test_what_runs_after_a_traversal_counts_from_its_entry(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>After</title>")
    script = """(async () => {
      const r = [];
      history.pushState({x: 1}, '');
      await new Promise(resolve => { addEventListener('popstate', resolve, {once: true}); history.back(); });
      r.push(history.state?.x ?? null, history.length);
      history.pushState({x: 2}, '');
      const key = navigation.entries()[0].key;
      await new Promise(resolve => { addEventListener('popstate', resolve, {once: true}); history.back(); });
      r.push(history.state?.x ?? null, history.length, navigation.currentEntry.index);
      history.pushState({x: 3}, '');
      r.push(await navigation.traverseTo(key).finished.then(e => 'traversed ' + e.index, e => e.name));
      r.push(history.state?.x ?? null);
      return r;
    })()"""

    with _open_page(session, url) as page:
        # The microtasks of a traversal's popstate run once it has committed, so a back() or traverseTo() there
        # counts from the entry that it went to, as WPT's back-pushstate-back-history-state does.
        assert page.evaluate(script) == [None, 2, None, 2, 0, "traversed 0", None]


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


def test_a_traversal_within_the_document_leaves_a_waiting_navigation_to_go_on(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/a", "<title>Waiting</title>")
    held = threading.Event()
    server.routes["/slow"] = Route(b"<title>Slow</title>", headers=[("Content-Type", HTML)], hold=held)
    slow = server.url("/slow")
    script = """window.marker = 1;
      history.pushState(null, '', '#x');
      location.href = '/slow';
      history.back();
      new Promise(r => setTimeout(() => r([location.href, history.length, window.marker]), 300))"""

    with _open_page(session, url) as page:
        # back() goes to the first entry while the navigation waits for its response, which then commits after it,
        # in place of the entry that the traversal left.
        assert page.evaluate(script) == [url, 2, 1]
        held.set()
        assert _next_document(page).url == slow
        assert page.evaluate("[history.length, navigation.entries().map(e => new URL(e.url).pathname)]") == [
            2,
            ["/a", "/slow"],
        ]


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


def test_a_traverse_to_the_entry_that_a_traversal_goes_to_finds_no_entry(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<title>Pending</title>")
    script = """(async () => {
      const r = [];
      const settle = p => p.then(() => 'fulfilled', e => 'rejected ' + e.name);
      history.pushState(null, '', '#a');
      let inner;
      navigation.addEventListener('navigate', e => {
        r.push('navigate ' + e.navigationType + ' ' + new URL(e.destination.url).hash);
        if (!inner) inner = navigation.traverseTo(e.destination.key);
      });
      history.back();
      await new Promise(resolve => setTimeout(resolve, 100));
      r.push(await settle(inner.committed), await settle(inner.finished), location.hash, navigation.currentEntry.index);
      return r;
    })()"""

    with _open_page(session, url) as page:
        # Chrome's controller looks for the key from the entry that the traversal goes to, which it leaves out, as
        # it does the current entry, so the traverseTo() that its navigate event's listener calls finds none.
        assert page.evaluate(script) == [
            "navigate traverse ",
            "rejected InvalidStateError",
            "rejected InvalidStateError",
            "",
            0,
        ]


def test_a_traversal_whose_entry_a_listener_drops_goes_nowhere(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Dropped</title>")
    script = """(async () => {
      const r = [];
      const settle = p => p.then(() => 'fulfilled', e => 'rejected ' + e.name);
      history.pushState(null, '', '#1');
      await new Promise(res => { addEventListener('popstate', res, {once: true}); history.back(); });
      navigation.addEventListener('navigate', e => {
        r.push('navigate ' + e.navigationType + ' ' + new URL(e.destination.url).hash);
        if (e.navigationType == 'replace') e.intercept({handler: () => new Promise(() => {})});
      });
      const first = navigation.navigate('#2', {history: 'replace'});
      first.committed.catch(() => {});
      first.finished.catch(() => {});
      navigation.addEventListener('navigateerror', e => {
        r.push('navigateerror ' + e.error.name);
        history.pushState(null, '', '#3');
      }, {once: true});
      addEventListener('popstate', () => r.push('popstate ' + location.hash));
      const result = navigation.forward();
      r.push(await settle(result.committed), await settle(result.finished));
      await new Promise(res => setTimeout(res, 200));
      r.push(location.hash, navigation.currentEntry.index, navigation.entries().map(e => new URL(e.url).hash));
      return r;
    })()"""

    with _open_page(session, url) as page:
        # The traversal aborts the navigation that its handler holds, whose navigateerror listener pushes an entry in
        # place of the one that the traversal goes to, which then goes nowhere. history.length is 2 here, where
        # Chrome's renderer copy of it gives 3, as it has taken the traversal's offset before the push.
        assert page.evaluate(script) == [
            "navigate replace #2",
            "navigateerror AbortError",
            "navigate push #3",
            "rejected AbortError",
            "rejected AbortError",
            "#3",
            1,
            ["#2", "#3"],
        ]


def test_update_current_entry_keeps_the_entry_it_was_called_for(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/a", "<title>Update</title>")
    server.page("/other", "<title>Other</title>")
    script = """(() => {
      const before = navigation.currentEntry;
      const r = [];
      navigation.addEventListener('currententrychange',
        e => r.push(['cec', e.from === before, location.hash]));
      navigation.updateCurrentEntry({state: {get s() { history.pushState(null, '', '#p'); return 1; }}});
      r.push(JSON.stringify(before.getState()), navigation.currentEntry === before,
             navigation.currentEntry.getState() === undefined, location.hash, navigation.currentEntry.index);
      return r;
    })()"""
    entries = (
        "navigation.entries().map(e => [new URL(e.url).pathname + new URL(e.url).hash, e.getState() === undefined])"
    )

    with _open_page(session, url) as page:
        # A getter of the state pushes an entry: the entry that was current takes the state, and the event names it,
        # but the session history keeps the state for no entry, as the entry is no longer the current one. The
        # event's navigationType is a Known Gap.
        assert page.evaluate(script) == [
            ["cec", True, "#p"],
            ["cec", True, "#p"],
            '{"s":1}',
            False,
            True,
            "#p",
            1,
        ]
        page.evaluate("location.href = '/other'; 0")
        _next_document(page)
        assert page.evaluate(entries) == [["/a", True], ["/a#p", True], ["/other", True]]


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


def test_a_document_whose_referrer_policy_hides_its_url_leaves_it_out_of_later_entries(
    session: mitto.Session, server: RecordingServer
) -> None:
    first = server.page("/norefer?q=1", "<title>No referrer</title>", [("Referrer-Policy", "no-referrer")])
    second = server.page("/b", "<title>B</title>")
    third = server.page("/c", "<title>C</title>")
    entries = (
        "[navigation.entries().map(e => e.url), navigation.activation.from === null,"
        " navigation.activation.from && navigation.activation.from.url]"
    )

    with _open_page(session, first) as page:
        # A document sees its own URLs.
        assert page.evaluate(entries) == [[first], True, None]
        page.evaluate("history.pushState(null, '', '/norefer?q=2'); location.href = '/b'; 0")
        assert _next_document(page).url == second
        # Later documents see each of its entries, without a URL, as Chrome's protect_url_in_navigation_api keeps
        # it from them.
        assert page.evaluate(entries) == [[None, None, second], False, None]
        page.evaluate("history.pushState(null, '', '/b2'); location.replace('/c'); 0")
        assert _next_document(page).url == third
        assert page.evaluate(entries) == [[None, None, second, third], False, server.url("/b2")]


def test_a_meta_referrer_policy_changes_only_the_current_entry(session: mitto.Session, server: RecordingServer) -> None:
    hidden = server.page("/norefer?q=1", "<title>No referrer</title>", [("Referrer-Policy", "no-referrer")])
    shown = server.page("/a", "<title>Shown</title>")
    later = server.page("/b", "<title>B</title>")
    meta = """(policy => {
      const meta = document.createElement('meta');
      meta.name = 'referrer';
      meta.content = policy;
      document.head.append(meta);
      return 0;
    })"""

    with _open_page(session, hidden) as page:
        # The entry that the document is at when its meta element changes the policy takes it, and the one before
        # keeps what the response's policy gave it.
        page.evaluate(f"history.pushState(null, '', '/norefer?q=2'); {meta}('unsafe-url')")
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == later
        assert page.evaluate("navigation.entries().map(e => e.url)") == [None, server.url("/norefer?q=2"), later]
    with _open_page(session, shown) as page:
        # An entry that the document makes after its meta element's policy takes the response's policy again.
        page.evaluate(
            f"history.pushState(null, '', '/a?2'); {meta}('no-referrer'); history.pushState(null, '', '/a?3')"
        )
        page.evaluate("location.href = '/b'; 0")
        assert _next_document(page).url == later
        assert page.evaluate("navigation.entries().map(e => e.url)") == [shown, None, server.url("/a?3"), later]


def test_an_entry_takes_the_navigation_api_state_as_chrome_carries_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/a", "<title>State</title>")
    script = """(async () => {
      const r = [];
      const settle = () => new Promise((resolve) => setTimeout(resolve, 50));
      navigation.updateCurrentEntry({state: {s: 1}});
      navigation.addEventListener('navigate', (e) => e.intercept(), {once: true});
      location.href = '/other';
      await settle();
      r.push(location.pathname, navigation.currentEntry.getState() === undefined, history.state);
      navigation.updateCurrentEntry({state: {s: 2}});
      navigation.addEventListener('navigate', (e) => e.intercept(), {once: true});
      location.hash = 'f';
      await settle();
      r.push(location.hash, navigation.currentEntry.getState() === undefined);
      navigation.updateCurrentEntry({state: {s: 3}});
      location.hash = 'g';
      await settle();
      r.push(location.hash, JSON.stringify(navigation.currentEntry.getState()));
      history.pushState({c: 1}, '', '/p');
      navigation.addEventListener('navigate', (e) => e.intercept(), {once: true});
      history.replaceState({c: 2}, '', '/q');
      await settle();
      r.push(location.pathname, JSON.stringify(history.state));
      return r;
    })()"""

    with _open_page(session, url) as page:
        # An intercepted navigation, to a path or a fragment, makes an entry of the history API, which takes no
        # Navigation API state from the one before; a navigation to a fragment that nothing intercepts takes it. An
        # intercepted replaceState() keeps its own state.
        assert page.evaluate(script) == ["/other", True, None, "#f", True, "#g", '{"s":3}', "/q", '{"c":2}']


def test_a_reload_that_a_redirect_takes_to_another_origin_keeps_nothing_of_the_entry(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/r", "<title>Reloaded</title>")
    other = server.page("/b", "<title>Other</title>").replace("127.0.0.1", "localhost")
    entry = (
        "[location.href, navigation.currentEntry.getState() === undefined, navigation.activation.navigationType, "
        "navigation.activation.from, history.length, performance.getEntriesByType('navigation')[0].type]"
    )

    with _open_page(session, url) as page:
        page.evaluate("navigation.updateCurrentEntry({state: {s: 1}})")
        server.routes["/r"] = Route(b"", 302, [("Location", other)])
        page.evaluate("location.reload()")
        assert _next(page).url == other
        # The reload's entry starts anew in the other origin: no state, and no entry that it came from.
        assert page.evaluate(entry) == [other, True, "reload", None, 1, "reload"]


def test_the_initial_about_blank_takes_another_query_and_fragment(session: mitto.Session) -> None:
    with _open_page(session, "about:blank") as page:
        # A relative URL does not resolve against about:blank, and an absolute one of the same path may change only
        # its query and fragment, which the Session takes.
        assert page.evaluate(
            "(() => { try { history.pushState(null, '', '?x'); return 'none'; } "
            "catch (e) { return e.name + ': ' + e.message; } })()"
        ) == (
            "SecurityError: Failed to execute 'pushState' on 'History': A history state object with URL '' cannot "
            "be created in a document with origin 'null' and URL 'about:blank'."
        )
        assert page.evaluate("history.replaceState(null, '', 'about:blank?y#z'); location.href") == "about:blank?y#z"
        assert page.evaluate("history.length") == 1


def test_a_traversal_gives_the_state_of_its_entry_anew(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", "<title>Anew</title>")
    script = """(async () => {
      history.replaceState({n: 1}, '');
      const changed = history.state;
      changed.n = 2;
      history.pushState({n: 3}, '', '#b');
      let popped;
      await new Promise(resolve => {
        addEventListener('popstate', e => { popped = e.state; resolve(); }, {once: true});
        history.back();
      });
      return [history.state, history.state === changed, popped === history.state, history.state === history.state];
    })()"""

    with _open_page(session, url) as page:
        # The entry's state as it was stored, not the object that the script changed before it left, and then the
        # same object for popstate and each read.
        assert page.evaluate(script) == [{"n": 1}, False, True, True]


def test_a_frame_has_the_history_of_its_tab(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/a", "<title>Frames</title>")
    opened = """(() => {
      const frame = document.createElement('iframe');
      document.body.append(frame);
      const child = frame.contentWindow;
      frame.contentDocument.open();
      frame.contentDocument.write('<p>x</p>');
      frame.contentDocument.close();
      const r = [child.location.href];
      child.history.replaceState(null, '');
      r.push(child.location.href);
      child.history.replaceState(null, '', '?y');
      r.push(child.location.href, child.history.length);
      return r;
    })()"""
    blank = """(() => {
      const frame = document.createElement('iframe');
      document.body.append(frame);
      try {
        frame.contentWindow.history.replaceState(null, '', 'about:blank?x');
        return 'none';
      } catch (e) {
        return [e.name + ': ' + e.message, frame.contentWindow.location.href];
      }
    })()"""

    with _open_page(session, url) as page:
        # document.open() gives the frame's about:blank the URL of the document that opens it, which the history
        # API may then change as that document's.
        assert page.evaluate(opened) == [url, url, url + "?y", 1]
        # An about:blank of its parent's origin, which is not opaque, keeps its query.
        assert page.evaluate(blank) == [
            "SecurityError: Failed to execute 'replaceState' on 'History': A history state object with URL "
            f"'about:blank?x' cannot be created in a document with origin '{server.origin}' and URL 'about:blank'.",
            "about:blank",
        ]


def test_a_document_that_document_open_gives_about_blank_may_keep_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/a", "<title>Opened</title>")
    script = """new Promise(resolve => {
      const frame = document.createElement('iframe');
      document.body.append(frame);
      window.done = resolve;
      frame.contentWindow.setTimeout(frame.contentWindow.Function(`
        const top = parent;
        const r = [top.location.href];
        top.document.open();
        top.document.write('<p>y</p>');
        top.document.close();
        r.push(top.location.href);
        top.history.replaceState(null, '');
        r.push(top.location.href, top.history.length);
        top.done(r);
      `), 0);
    })"""

    with _open_page(session, url) as page:
        # The frame's about:blank, run by its own timer, opens its parent's document, which takes the frame's URL,
        # and the history API may then keep it.
        assert page.evaluate(script) == [url, "about:blank", "about:blank", 1]
        assert page.evaluate("location.href") == "about:blank"


def test_each_frame_has_its_own_rate_limit(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/a", "<title>Limit</title>")
    script = """(() => {
      const frame = document.createElement('iframe');
      document.body.append(frame);
      const child = frame.contentWindow;
      for (let i = 0; i < 200; i++) child.history.replaceState(i, '');
      history.pushState(null, '', '#after');
      const r = [location.hash, history.length, child.history.state];
      child.history.replaceState('late', '');
      r.push(child.history.state);
      return r;
    })()"""

    with _open_page(session, url) as page:
        # The frame's 200 calls leave the top-level frame's allowance whole, and its own 201st does nothing but warn.
        assert page.evaluate(script) == ["#after", 2, 199, 199]
        assert page.console_messages == [
            _native.ConsoleMessage(
                level="warning",
                text="Throttling navigation to prevent the browser from hanging. See https://crbug.com/1038223. Command "
                "line switch --disable-ipc-flooding-protection can be used to bypass the protection",
            )
        ]


def test_what_an_unload_listener_does_to_the_history_goes_nowhere(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/a", "<title>Unload</title>")
    second = server.page("/b", "<title>B</title>")
    listen = "addEventListener('pagehide', () => history.pushState(null, '', '#p'));"
    entries = (
        "[history.length, navigation.entries().map(e => new URL(e.url).pathname + new URL(e.url).hash),"
        " new Set(navigation.entries().map(e => e.key)).size, performance.getEntriesByType('navigation')[0].type]"
    )

    with _open_page(session, url) as page:
        # A pagehide listener's pushState() as a new document commits makes no entry, as Chrome's browser ignores the
        # document that it replaces, for a reload as for a new entry.
        page.evaluate(f"{listen} location.reload(); 0")
        assert _next_document(page).url == url
        assert page.evaluate(entries) == [1, ["/a"], 1, "reload"]
        page.evaluate(f"{listen} location.href = '/b'; 0")
        assert _next_document(page).url == second
        assert page.evaluate(entries) == [2, ["/a", "/b"], 2, "navigate"]


def test_a_long_value_that_an_enumeration_lacks_warns_up_to_the_console_limit(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<title>Long</title>")
    longest = 16 * 1024 * 1024 - 1

    with _open_page(session, url) as page:
        # The warning holds the value, cut to what a console message carries, and the Page goes on.
        assert page.evaluate("const h = new Highlight(); h.type = 'x'.repeat(16 * 1024 * 1024); h.type") == "highlight"
        [message] = page.console_messages
        assert (message.level, len(message.text.encode()), message.text[:24]) == (
            "warning",
            longest,
            "The provided value 'xxxx",
        )


def test_a_value_of_an_enumeration_that_it_lacks_warns_in_the_console(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<title>Enumeration</title>")

    with _open_page(session, url) as page:
        # A Known Gap's setter warns as an implemented attribute's does, as every bind_gen setter does in Chrome.
        assert page.evaluate("const h = new Highlight(); h.type = 'bogus'; h.type") == "highlight"
        assert page.console_messages == [
            _native.ConsoleMessage(
                level="warning", text="The provided value 'bogus' is not a valid enum value of type HighlightType."
            )
        ]
