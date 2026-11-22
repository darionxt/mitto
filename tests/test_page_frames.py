"""Tests of a Page's frames, on a local server.

tests/frames_cases.js runs in a Page at an HTTP origin and gives what its iframes did without the network: the initial
about:blank and the about:blank that a frame commits at once, the frame tree on the window, contentWindow and
contentDocument, realms, writing into a frame's document, named and indexed access, and a frame that goes.
tests/data/frames_cases.json holds what Chrome 157 gave for the same script on the same kind of page, headless Chrome for
Testing on macOS (157.0.8103.79), with the page's origin as ORIGIN. The other tests check what a frame reports and what
the Session hears of a frame's document.
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
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_cases.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_cases.json").read_text())
_SRCDOC_CASES = (_TESTS / "frames_srcdoc_cases.js").read_text()
_SRCDOC_AS_CHROME = json.loads((_TESTS / "data" / "frames_srcdoc_cases.json").read_text())


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of a page with a paragraph on 127.0.0.1, which is a secure context, where /from-... answers with its
    request's Referer and /framed is a page whose markup holds an iframe of another URL."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            # A path that starts with /from- answers with the Referer that came, or "none".
            if self.path.startswith("/from-"):
                body, content_type = (self.headers.get("Referer") or "none").encode(), "text/plain"
            else:
                body, content_type = (_FRAMED_PAGE if self.path == "/framed" else _PAGE), "text/html"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
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
