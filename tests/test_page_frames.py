"""Tests of a Page's frames, on a local server.

tests/frames_cases.js runs in a Page at an HTTP origin and gives what its iframes did without the network: the initial
about:blank and the about:blank that a frame commits at once, the frame tree on the window, contentWindow and
contentDocument, realms, writing into a frame's document, named and indexed access, and a frame that goes.
tests/data/frames_cases.json holds what Chrome 157 gave for the same script on the same kind of page, headless Chrome for
Testing on macOS (157.0.8103.79), with the page's origin as ORIGIN. The other tests check what a frame reports, what the
Session hears of a frame's document, and what frames do as they go or move, where the expected values are what the same
Chrome gave for the same scripts.
"""

import http.server
import json
import threading
from collections.abc import Iterator
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


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of a page with a paragraph on 127.0.0.1, which is a secure context, where /framed is a page whose markup
    holds an iframe of another URL, and /opened one whose script opens a frame's document and removes the frame a second
    later."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            body = {"/framed": _FRAMED_PAGE, "/opened": _OPENED_PAGE}.get(self.path, _PAGE)
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
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
    # the same script: a frame's function that another frame's requestAnimationFrame(), timer or mutation observer
    # runs fires the error event of its own frame's window.
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
        new windows[0].MutationObserver(new windows[2].Function('throw new Error("records")'))
            .observe(document.body, {childList: true});
        document.body.appendChild(document.createElement('div'));
        await new Promise((resolve) => setTimeout(resolve, 200));
        return calls.sort();
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["frame1", "frame1", "frame2"]


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
