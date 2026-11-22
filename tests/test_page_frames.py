"""Tests of a Page's frames, on a local server.

tests/frames_cases.js runs in a Page at an HTTP origin and gives what its iframes did without the network: the initial
about:blank and the about:blank that a frame commits at once, the frame tree on the window, contentWindow and
contentDocument, realms, writing into a frame's document, named and indexed access, and a frame that goes.
tests/data/frames_cases.json holds what Chrome 157 gave for the same script on the same kind of page, headless Chrome for
Testing on macOS (157.0.8103.79), with the page's origin as ORIGIN. tests/frames_srcdoc_cases.js and
tests/frames_navigation_cases.js do the same for srcdoc and later about:blank documents, and for frames that navigate
through the Session. The other tests check what a frame reports and what the Session hears of a frame's document.
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
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_cases.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_cases.json").read_text())
_SRCDOC_CASES = (_TESTS / "frames_srcdoc_cases.js").read_text()
_SRCDOC_AS_CHROME = json.loads((_TESTS / "data" / "frames_srcdoc_cases.json").read_text())
_NAVIGATION_CASES = (_TESTS / "frames_navigation_cases.js").read_text()
_NAVIGATION_AS_CHROME = json.loads((_TESTS / "data" / "frames_navigation_cases.json").read_text())
# A frame's document whose script reads performance.timing while it parses.
_TIMING_PAGE = b"""<!doctype html><script>
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
_ECHOED = ("accept", "referer", "sec-fetch-dest", "sec-fetch-mode", "sec-fetch-site", "sec-fetch-user",
           "upgrade-insecure-requests")


def answer(path: str, headers: Mapping[str, str]) -> tuple[int, dict[str, str], bytes]:
    """The status, headers and body with which the frames' server answers a GET of `path`.

    /from-... gives the request's Referer, or "none"; /echo the headers in _ECHOED that came, in a script element of id h;
    /204 nothing; /redirect a redirect to /child?redirected; /drop no response at all, as status 0; /timing a document
    whose script puts what it reads of performance.timing into parent.timingAtParse; /slow, after 300 ms, and /child... and /dir/... a document
    whose paragraph of id c holds its path; /nested a document with a frame of /slow?inner, whose load and its own go
    into top.order; anything else the top-level page.
    """
    html_type = {"Content-Type": "text/html"}
    if path.startswith("/from-"):
        return 200, {"Content-Type": "text/plain"}, (headers.get("referer") or "none").encode()
    if path == "/echo":
        echoed = {name: headers[name] for name in _ECHOED if name in headers}
        script = html.escape(json.dumps(echoed, sort_keys=True), quote=False)
        return 200, html_type, f'<!doctype html><script type=application/json id=h>{script}</script>'.encode()
    if path == "/204":
        return 204, {}, b""
    if path == "/drop":
        return 0, {}, b""
    if path == "/timing":
        return 200, html_type, _TIMING_PAGE
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
        record = page.evaluate(_NAVIGATION_CASES)
    origin = page_url.rstrip("/")
    as_chrome = json.loads(json.dumps(_NAVIGATION_AS_CHROME["record"]).replace("ORIGIN", origin))

    assert set(record) == set(as_chrome)
    for section, expected in as_chrome.items():
        assert record[section] == expected, section


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
