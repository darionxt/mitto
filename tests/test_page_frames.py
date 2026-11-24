"""Tests of a Page's frames, on a local server.

tests/frames_cases.js runs in a Page at an HTTP origin and gives what its iframes did without the network: the initial
about:blank and the about:blank that a frame commits at once, the frame tree on the window, contentWindow and
contentDocument, realms, writing into a frame's document, named and indexed access, and a frame that goes.
tests/data/frames_cases.json holds what Chrome 157 gave for the same script on the same kind of page, headless Chrome for
Testing on macOS (157.0.8103.79), with the page's origin as ORIGIN. tests/frames_srcdoc_cases.js and
tests/frames_navigation_cases.js do the same for srcdoc and later about:blank documents, and for frames that navigate
through the Session. The other tests check what a frame reports, what the Session hears of a frame's document, and what
frames do as they go or move, where the expected values are what the same Chrome gave for the same scripts.
"""

import html
import http.server
import json
import threading
import time
from collections.abc import Iterator, Mapping
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_PAGE = b"<!doctype html><title>t</title><body><p id=x>top</p>"
_FRAMED_PAGE = b"<!doctype html><title>t</title><body><iframe src=/framed-child></iframe>"
_OPENED_PAGE = b"""<!doctype html><title>t</title><body><script>
  const element = document.createElement('iframe');
  document.body.appendChild(element);
  element.contentDocument.open();
  element.contentDocument.write('<p>');
  window.removedAt = -1;
  setTimeout(() => { removedAt = performance.now(); element.remove(); }, 1000);
</script>"""
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_cases.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_cases.json").read_text())
_SRCDOC_CASES = (_TESTS / "frames_srcdoc_cases.js").read_text()
_SRCDOC_AS_CHROME = json.loads((_TESTS / "data" / "frames_srcdoc_cases.json").read_text())
_NAVIGATION_CASES = (_TESTS / "frames_navigation_cases.js").read_text()
_NAVIGATION_AS_CHROME = json.loads((_TESTS / "data" / "frames_navigation_cases.json").read_text())
# A frame's document whose script reads performance.timing while it parses.
# Its script runs once the response has ended, after a script that comes late: a body's end is a message of its own, in
# Chrome too, which a script that runs as soon as the body has come may precede.
_TIMING_PAGE = b"""<!doctype html><script src=/hold.js></script><script>
const t = performance.timing;
parent.timingAtParse = {
  domLoading: t.domLoading >= t.navigationStart && t.domLoading > 0,
  domInteractive: t.domInteractive,
  domContentLoadedEventStart: t.domContentLoadedEventStart,
  domComplete: t.domComplete,
  loadEventStart: t.loadEventStart,
  loadEventEnd: t.loadEventEnd,
  fetchStart: t.fetchStart >= t.navigationStart,
  responseEnd: t.responseEnd >= t.fetchStart,
  entries: performance.getEntriesByType('navigation').length,
};
</script>"""
# The request headers that /echo gives back, which a frame's navigation sends.
_ECHOED = (
    "accept",
    "referer",
    "sec-fetch-dest",
    "sec-fetch-mode",
    "sec-fetch-site",
    "sec-fetch-user",
    "upgrade-insecure-requests",
)


def answer(path: str, headers: Mapping[str, str]) -> tuple[int, dict[str, str], bytes]:
    """The status, headers and body with which the frames' server answers a GET of `path`.

    /from-... gives the request's Referer, or "none"; /echo the headers in _ECHOED that came, in a script element of id h;
    /204 nothing; /redirect a redirect to /child?redirected; /drop no response at all, as status 0; /timing a document
    whose script puts what it reads of performance.timing into parent.timingAtParse, after /hold.js, an empty script
    that comes after 300 ms; /framed a page whose markup holds an iframe of /framed-child; /opened a page whose script
    opens a frame's document and removes the frame a second later; /slow, after 300 ms, and /child... and /dir/... a
    document whose paragraph of id c holds its path; /nested a document with a frame of /slow?inner, whose load and its
    own go into top.order; anything else the top-level page.
    """
    html_type = {"Content-Type": "text/html"}
    if path.startswith("/from-"):
        return 200, {"Content-Type": "text/plain"}, (headers.get("referer") or "none").encode()
    if path == "/echo":
        echoed = {name: headers[name] for name in _ECHOED if name in headers}
        script = html.escape(json.dumps(echoed, sort_keys=True), quote=False)
        return 200, html_type, f"<!doctype html><script type=application/json id=h>{script}</script>".encode()
    if path == "/204":
        return 204, {}, b""
    if path == "/drop":
        return 0, {}, b""
    if path == "/framed":
        return 200, html_type, _FRAMED_PAGE
    if path == "/opened":
        return 200, html_type, _OPENED_PAGE
    if path == "/timing":
        return 200, html_type, _TIMING_PAGE
    if path == "/hold.js":
        time.sleep(0.3)
        return 200, {"Content-Type": "text/javascript"}, b""
    if path == "/redirect":
        return 302, {"Location": "/child?redirected"}, b""
    if path == "/nested":
        body = (
            "<!doctype html><body onload=\"top.order.push('nested ' + document.readyState)\">"
            "<iframe src=/slow?inner onload=\"top.order.push('inner ' + this.contentDocument.readyState)\"></iframe>"
        )
        return 200, html_type, body.encode()
    if path.startswith(("/slow", "/child", "/dir/")):
        if path.startswith("/slow"):
            time.sleep(0.3)
        return 200, html_type, f"<!doctype html><title>child</title><p id=c>{html.escape(path)}</p>".encode()
    return 200, html_type, _PAGE


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of a page with a paragraph on 127.0.0.1, which is a secure context, on a server that answers as answer()
    does."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            status, headers, body = answer(self.path, {name.lower(): value for name, value in self.headers.items()})
            if not status:
                self.close_connection = True
                return
            self.send_response(status)
            for name, value in headers.items():
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_frames_without_the_network_are_chrome_s(page_url: str) -> None:
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES)
    origin = page_url.rstrip("/")
    as_chrome = json.loads(json.dumps(_AS_CHROME["record"]).replace("ORIGIN", origin))

    assert set(record) == set(as_chrome)
    for section, expected in as_chrome.items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_srcdoc_and_about_blank_frames_are_chrome_s(page_url: str) -> None:
    # tests/frames_srcdoc_cases.js against tests/data/frames_srcdoc_cases.json, as Chrome for Testing 157 gave it.
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_SRCDOC_CASES)
    origin = page_url.rstrip("/")
    as_chrome = json.loads(json.dumps(_SRCDOC_AS_CHROME["record"]).replace("ORIGIN", origin))

    assert set(record) == set(as_chrome)
    for section, expected in as_chrome.items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_frames_that_navigate_through_the_session_are_chrome_s(page_url: str) -> None:
    # tests/frames_navigation_cases.js against tests/data/frames_navigation_cases.json, as Chrome for Testing 157 gave
    # it on a server that answers as answer() does.
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_NAVIGATION_CASES, timeout=_TIMEOUT_SECONDS)
    origin = page_url.rstrip("/")
    as_chrome = json.loads(json.dumps(_NAVIGATION_AS_CHROME["record"]).replace("ORIGIN", origin))

    assert set(record) == set(as_chrome)
    for section, expected in as_chrome.items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_a_page_whose_markup_holds_a_frame_of_another_url_loads(page_url: str) -> None:
    # A frame's initial about:blank holds back no load, as Blink's FrameLoader::Init() suppresses its finish: the
    # parent's load waits only for a frame's navigation that waits, or for a document of the frame that loads.
    with mitto.Session() as session, session._open_page(page_url + "framed", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate("[document.readyState, frames.length]") == ["complete", 1]


@pytest.mark.usefixtures("page_program")
def test_an_indexeddb_open_of_a_frame_that_goes_lets_its_connection_go(page_url: str) -> None:
    # Each frame asks for a database and goes before the open gives its connection, which the library then closes from
    # a task of its own, not inside the upgrade that it runs: the Session goes on, and the top-level document still
    # opens a database.
    script = """
      (async () => {
        for (let i = 0; i < 20; i++) {
          const element = document.createElement('iframe');
          document.body.appendChild(element);
          element.contentWindow.indexedDB.open('gone-' + i);
          element.remove();
        }
        await new Promise((resolve) => setTimeout(resolve, 500));
        return await new Promise((resolve) => {
          const request = indexedDB.open('after');
          request.onsuccess = () => resolve(request.result.name);
          request.onerror = () => resolve('error');
        });
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == "after"


@pytest.mark.usefixtures("page_program")
def test_a_callback_s_exception_goes_to_its_own_realm(page_url: str) -> None:
    # Blink's InvokeAndReportException() reports with the callback's relevant ScriptState, which Chrome 157 gave for
    # the same script: a frame's function that another frame's requestAnimationFrame(), timer, idle callback or
    # mutation observer runs fires the error event of its own frame's window.
    script = """
      (async () => {
        const windows = [0, 1, 2].map(() => {
          const element = document.createElement('iframe');
          document.body.appendChild(element);
          return element.contentWindow;
        });
        const calls = [];
        window.onerror = () => { calls.push('top'); return true; };
        windows.forEach((w, i) => { w.onerror = () => { calls.push('frame' + i); return true; }; });
        windows[0].requestAnimationFrame(new windows[1].Function('throw new parent.frames[2].Error("raf")'));
        windows[0].setTimeout(new windows[1].Function('throw new Error("timer")'), 0);
        windows[0].requestIdleCallback(new windows[2].Function('throw new Error("idle")'));
        new windows[0].MutationObserver(new windows[2].Function('throw new Error("records")'))
            .observe(document.body, {childList: true});
        document.body.appendChild(document.createElement('div'));
        await new Promise((resolve) => setTimeout(resolve, 200));
        return calls.sort();
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["frame1", "frame1", "frame2", "frame2"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_document_reads_its_parent_s_cookies_and_goes_with_its_element(page_url: str) -> None:
    # The Session hears of the frame's document under the top-level one, and of its going once its element leaves the
    # document: the frame's document.cookie reads the parent's cookies through the Session while it lives.
    script = """
      document.cookie = 'shared=yes';
      const element = document.createElement('iframe');
      document.body.appendChild(element);
      const d = element.contentDocument;
      const before = d.cookie;
      d.cookie = 'from_frame=1';
      element.remove();
      [before, document.cookie.split('; ').sort()]
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["shared=yes", ["from_frame=1", "shared=yes"]]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_frame_sends_its_parent_s_referrer_and_an_about_blank_frame_none(page_url: str) -> None:
    # Blink's OutgoingReferrer(): a srcdoc document's is its parent's URL, and about:blank gives none, as Chrome 157
    # sent them (#261). The Session works out both from the documents it heard of.
    script = """
      (async () => {
        const srcdoc = document.createElement('iframe');
        srcdoc.srcdoc = '<script>parent.fromSrcdoc = fetch("/from-srcdoc").then(r => r.text());<\\/script>';
        await new Promise((resolve) => { srcdoc.onload = resolve; document.body.appendChild(srcdoc); });
        const blank = document.createElement('iframe');
        document.body.appendChild(blank);
        const fromBlank = blank.contentWindow.eval('fetch("/from-blank").then(r => r.text())');
        return [await window.fromSrcdoc, await fromBlank];
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [page_url, "none"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_removes_itself_as_its_parent_goes_goes_then(page_url: str) -> None:
    # The inner frame's pagehide listener removes the inner frame's element while the outer frame goes: the inner frame
    # goes inside that removal, its own child with it, and the outer one goes on with the rest.
    script = """
      const outer = document.createElement('iframe');
      document.body.appendChild(outer);
      const inner = outer.contentDocument.createElement('iframe');
      outer.contentDocument.body.appendChild(inner);
      const deeper = inner.contentDocument.createElement('iframe');
      inner.contentDocument.body.appendChild(deeper);
      const log = [];
      outer.contentWindow.addEventListener('pagehide', () => log.push('outer pagehide'));
      inner.contentWindow.addEventListener('pagehide', () => {
        log.push('inner pagehide');
        inner.remove();
        log.push('inner removed');
      });
      inner.contentWindow.addEventListener('unload', () => log.push('inner unload'));
      deeper.contentWindow.addEventListener('pagehide', () => log.push('deeper pagehide'));
      outer.remove();
      [log, outer.contentWindow, inner.contentWindow, inner.isConnected, frames.length]
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [
            ["outer pagehide", "inner pagehide", "deeper pagehide", "inner removed"],
            None,
            None,
            False,
            0,
        ]
        assert page.evaluate("document.title") == "t"


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_its_own_timer_removes_goes_then(page_url: str) -> None:
    # A frame's setTimeout() callback and its setInterval() callback each remove the frame and throw: the timer
    # returns, the interval runs no more, the frame's window sets no timer, and each error goes to the callback's
    # window, as Chrome 157 gave for the same script.
    script = """
      (async () => {
        const errors = [];
        window.onerror = (message) => { errors.push(message); return true; };
        const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
        const timeout = document.createElement('iframe');
        document.body.appendChild(timeout);
        const timeoutWindow = timeout.contentWindow;
        const set = await new Promise((resolve) => timeoutWindow.setTimeout(() => {
          timeout.remove();
          resolve(timeoutWindow.setTimeout(() => {}, 0));
          throw new Error('timeout');
        }, 0));
        const interval = document.createElement('iframe');
        document.body.appendChild(interval);
        let runs = 0;
        await new Promise((resolve) => interval.contentWindow.setInterval(() => {
          runs += 1;
          if (runs === 2) {
            interval.remove();
            resolve();
            throw new Error('interval');
          }
        }, 0));
        await wait(100);
        return [set, timeout.contentWindow, runs, errors, frames.length];
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [
            0,
            None,
            2,
            ["Uncaught Error: timeout", "Uncaught Error: interval"],
            0,
        ]
        assert page.evaluate("document.title") == "t"


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_its_own_callback_removes_runs_nothing_more(page_url: str) -> None:
    # A frame's animation frame callback and its idle callback each remove the frame and throw, and a callback that
    # waits for the same animation frame, the frame's own or one of a frame that the parent's removes, never runs, as
    # Chrome 157 gave for the same script. A fetch() of a frame that goes once it has its body leaves the Page as it
    # was.
    script = """
      (async () => {
        const errors = [];
        window.onerror = (message) => { errors.push(message); return true; };
        const frame = () => {
          const element = document.createElement('iframe');
          document.body.appendChild(element);
          return element;
        };
        const ran = [];
        const own = frame();
        await new Promise((resolve) => {
          own.contentWindow.requestAnimationFrame(() => {
            ran.push('first');
            own.remove();
            setTimeout(resolve, 100);
            throw new Error('raf');
          });
          own.contentWindow.requestAnimationFrame(() => ran.push('second'));
        });
        const child = frame();
        await new Promise((resolve) => {
          requestAnimationFrame(() => {
            ran.push('parent');
            child.remove();
            setTimeout(resolve, 100);
          });
          child.contentWindow.requestAnimationFrame(() => ran.push('child'));
        });
        const idle = frame();
        await new Promise((resolve) => {
          idle.contentWindow.requestIdleCallback(() => {
            ran.push('idle');
            idle.remove();
            setTimeout(resolve, 100);
            throw new Error('idle');
          });
        });
        const fetched = frame();
        const text = await fetched.contentWindow.fetch('/fetched').then((response) => response.text());
        fetched.remove();
        return [ran, errors, text.length > 0, frames.length];
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [
            ["first", "parent", "idle"],
            ["Uncaught Error: raf", "Uncaught Error: idle"],
            True,
            0,
        ]
        assert page.evaluate("document.title") == "t"


@pytest.mark.usefixtures("page_program")
def test_a_nested_frame_s_storage_outlives_its_ancestor_s_removal(page_url: str) -> None:
    # The nested frame's Cache and storage bucket keep what they call once the outer frame goes, though the Session
    # hears nothing more of the frame's document, and the top-level document's storage still works.
    script = """
      (async () => {
        const outer = document.createElement('iframe');
        document.body.appendChild(outer);
        const inner = outer.contentDocument.createElement('iframe');
        outer.contentDocument.body.appendChild(inner);
        const cache = await inner.contentWindow.caches.open('nested');
        const bucket = await inner.contentWindow.navigator.storageBuckets.open('nested');
        outer.remove();
        cache.keys().catch(() => {});
        bucket.persisted().catch(() => {});
        await new Promise((resolve) => setTimeout(resolve, 200));
        const top = await caches.open('top');
        await top.put('/x', new Response('x'));
        return (await top.keys()).length;
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == 1


@pytest.mark.usefixtures("page_program")
def test_a_page_loads_once_a_frame_whose_opened_document_still_loads_goes(page_url: str) -> None:
    # The frame's document that open() made and nothing closed holds the page's load event back until the frame goes.
    with mitto.Session() as session, session._open_page(page_url + "opened", timeout=20) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(
            "[document.readyState, frames.length, removedAt > 0,"
            " performance.getEntriesByType('navigation')[0].loadEventStart >= removedAt]"
        ) == ["complete", 0, True, True]


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_move_before_moves_keeps_its_window(page_url: str) -> None:
    # moveBefore() keeps the frame's window, whether it moves the frame's element or an ancestor of it.
    script = """
      const element = document.createElement('iframe');
      document.body.appendChild(element);
      const w = element.contentWindow;
      w.marker = 7;
      const holder = document.createElement('div');
      document.body.appendChild(holder);
      holder.moveBefore(element, null);
      const moved = [element.contentWindow === w, element.contentWindow?.marker, w.closed];
      document.body.moveBefore(holder, document.body.firstChild);
      [moved, element.contentWindow === w, w.closed, frames.length]
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [[True, 7, False], True, False, 1]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_about_blank_takes_its_creator_s_base_url(page_url: str) -> None:
    # The initial about:blank's base URL is its creator's at the time, and the about:blank that the frame then commits
    # at once keeps it, though the creator's base URL has changed since.
    script = """
      const base = document.createElement('base');
      base.href = '/sub/dir/';
      document.head.appendChild(base);
      const element = document.createElement('iframe');
      document.body.appendChild(element);
      const initial = element.contentDocument.baseURI;
      base.href = '/other/';
      const kept = element.contentDocument.baseURI;
      element.src = 'about:blank';
      const anchor = element.contentDocument.createElement('a');
      anchor.href = 'rel';
      [initial, kept, element.contentDocument.baseURI, element.contentDocument.URL, anchor.href]
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        origin = page_url.rstrip("/")
        assert page.evaluate(script) == [
            f"{origin}/sub/dir/",
            f"{origin}/sub/dir/",
            f"{origin}/sub/dir/",
            "about:blank",
            f"{origin}/sub/dir/rel",
        ]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_cookie_store_and_its_parent_s_each_hear_of_changes(page_url: str) -> None:
    # Each document's CookieStore listens on its own, so a frame's listener takes nothing from its parent's.
    script = """
      (async () => {
        const element = document.createElement('iframe');
        document.body.appendChild(element);
        const seen = [];
        cookieStore.onchange = (e) => seen.push('top ' + e.changed.map((c) => c.name).join());
        element.contentWindow.cookieStore.onchange = (e) => seen.push('frame ' + e.changed.map((c) => c.name).join());
        await new Promise((resolve) => setTimeout(resolve, 300));
        await cookieStore.set('from_top', '1');
        await new Promise((resolve) => setTimeout(resolve, 500));
        await element.contentWindow.cookieStore.set('from_frame', '2');
        await new Promise((resolve) => setTimeout(resolve, 500));
        return seen.sort();
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["frame from_frame", "frame from_top", "top from_frame", "top from_top"]


@pytest.mark.usefixtures("page_program")
def test_a_page_s_own_members_do_not_reach_a_frame_of_another_origin(page_url: str) -> None:
    # A getter, a setter or a method of the page's window, called on the window of a frame of another origin, throws, as
    # in Chrome 157 but for the members that another origin may reach, and so does postMessage(), which Chrome sends: a
    # message of another origin names its target as its source yet, a Known Gap.
    script = """
      (async () => {
        const threw = (f) => { try { f(); return false; } catch (e) { return true; } };
        const element = document.createElement('iframe');
        element.src = `http://localhost:${location.port}/child?other`;
        document.body.appendChild(element);
        await new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
        const w = element.contentWindow;
        const get = (name) => Object.getOwnPropertyDescriptor(window, name).get.call(w);
        const set = (name, value) => Object.getOwnPropertyDescriptor(window, name).set.call(w, value);
        const refused = {
          document: () => get('document'), reflect: () => Reflect.get(window, 'document', w), location: () => get('location').href,
          localStorage: () => get('localStorage'), name: () => get('name'), origin: () => get('origin'), navigator: () => get('navigator'),
          frameElement: () => get('frameElement'), onload: () => get('onload'), setName: () => set('name', 'x'),
          setOnload: () => set('onload', () => 1), setSelf: () => set('self', 1), setOpener: () => set('opener', 1),
          fetch: () => fetch.call(w, '/'), setTimeout: () => setTimeout.call(w, () => 1), btoa: () => btoa.call(w, 'a'),
          structuredClone: () => structuredClone.call(w, 1), stop: () => stop.call(w), open: () => open.call(w, 'about:blank', '_self'),
          addEventListener: () => addEventListener.call(w, 'x', () => 1), dispatchEvent: () => dispatchEvent.call(w, new Event('x')),
          getComputedStyle: () => getComputedStyle.call(w, document.body), property: () => w.document,
          postMessage: () => postMessage.call(w, 1, '*'),
        };
        const reached = {
          window: () => get('window') === w, self: () => get('self') === w, frames: () => get('frames') === w,
          parent: () => get('parent') === window, top: () => get('top') === window, opener: () => get('opener') === null,
          length: () => get('length') === 0, closed: () => get('closed') === false, blur: () => blur.call(w) === undefined,
        };
        const notRefused = Object.keys(refused).filter((name) => !threw(refused[name]));
        const notReached = Object.keys(reached).filter((name) => threw(reached[name]) || !reached[name]());
        return [element.contentDocument, notRefused, notReached];
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [None, [], []]


@pytest.mark.usefixtures("page_program")
def test_a_window_that_its_frame_left_reaches_only_a_window_of_its_origin(page_url: str) -> None:
    # A function of a window that its frame navigated away from takes the frame's current window for its bare members,
    # which it may use in a window of its own origin, and not in another's, and has no named elements, as in Chrome 157.
    script = """
      (async () => {
        const tried = (f) => { try { return f(); } catch (e) { return e.name === 'ReferenceError' ? e.message : 'refused'; } };
        const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
        const results = {};
        for (const origin of ['', `http://localhost:${location.port}`]) {
          const element = document.createElement('iframe');
          element.name = 'n';
          element.src = '/child?old';
          document.body.appendChild(element);
          await loaded(element);
          const w = element.contentWindow;
          w.eval("var mine = 'old'");
          const f = w.eval(`[
            () => c.localName, () => mine, () => window.document.URL, () => document.URL.includes('old') || document.URL,
            () => name, () => origin, () => typeof setTimeout(() => 1), () => typeof localStorage, () => location.pathname,
            () => 'document' in self, () => self === window, () => parent === top, () => length, () => closed,
          ]`);
          const before = f[0]();
          element.src = origin + '/child?new';
          await loaded(element);
          results[origin ? 'other' : 'same'] = [before, ...f.map(tried)];
          element.remove();
        }
        return results;
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        results = page.evaluate(script)
    # What Chrome's `document` gives, the document that the window had, is a Known Gap: the frame's current document, if
    # the window may access it, and otherwise it is refused.
    new = f"{page_url}child?new"
    assert results["same"] == [
        *("p", "c is not defined", "old", new, new),
        *("n", page_url.rstrip("/"), "number", "object", "/child", True, True, True, 0, False),
    ]
    assert results["other"] == [
        *("p", "c is not defined", "old", "refused", "refused"),
        *("refused", "refused", "refused", "refused", "refused", "refused", True, True, 0, False),
    ]


@pytest.mark.usefixtures("page_program")
def test_a_page_of_an_opaque_origin_is_of_its_own_origin() -> None:
    # The top-level about:blank, whose origin is opaque, opens and writes its own document, and reaches the document of
    # a frame that took its origin.
    script = """
      const element = document.createElement('iframe');
      document.body.appendChild(element);
      const reached = element.contentDocument !== null;
      document.open();
      document.write('<p>x');
      document.close();
      [reached, document.body.innerHTML]
    """
    with mitto.Session() as session, session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [True, "<p>x</p>"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_of_its_parent_s_opaque_origin_is_reached_through_its_window() -> None:
    # A frame that took the top-level about:blank's opaque origin is of that origin through its window too: its parent
    # reaches its document and globals, and it its parent's, and so do its own frame and a sibling, as in Chrome 157.
    script = """
      const tried = (f) => { try { return f(); } catch (e) { return e.name + ': ' + e.message; } };
      const element = document.createElement('iframe');
      document.body.appendChild(element);
      const w = element.contentWindow;
      const sibling = document.createElement('iframe');
      document.body.appendChild(sibling);
      [
        tried(() => w.origin),
        tried(() => w.document === element.contentDocument),
        tried(() => { w.fromTop = 5; return w.eval('fromTop + 1'); }),
        tried(() => w.eval('[typeof parent.document, parent.document === top.document]')),
        tried(() => w.eval(`
          const inner = document.createElement('iframe');
          document.body.appendChild(inner);
          inner.contentWindow.k = 1;
          [inner.contentWindow.document === inner.contentDocument, inner.contentWindow.eval('[top.document.body.localName, k]')]
        `)),
        tried(() => sibling.contentWindow.eval('parent.frames[0].document === parent.document.querySelector("iframe").contentDocument')),
      ]
    """
    with mitto.Session() as session, session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["null", True, 6, ["object", True], [True, ["body", 1]], True]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_later_documents_of_its_parent_s_opaque_origin_are_reached_through_its_window() -> None:
    # Under the top-level about:blank, a frame's second srcdoc and the about:blank after it each come in a new window of
    # the parent's opaque origin, which the parent reaches, and which reaches the parent, as in Chrome 157.
    script = """
      (async () => {
        const tried = (f) => { try { return f(); } catch (e) { return e.name + ': ' + e.message; } };
        const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        const first = tried(() => [element.contentWindow.document.body.textContent, element.contentWindow.origin]);
        element.srcdoc = '<p>b';
        await loaded(element);
        const second = tried(() => [
          element.contentWindow.document.body.textContent, element.contentWindow.eval('parent.document.body.localName'),
        ]);
        element.removeAttribute('srcdoc');
        await loaded(element);
        const blank = tried(() => [element.contentWindow.document.URL, element.contentWindow.eval('parent.document === top.document')]);
        return [first, second, blank];
      })()
    """
    with mitto.Session() as session, session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == [["a", "null"], ["b", "body"], ["about:blank", True]]
