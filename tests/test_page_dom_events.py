"""Tests of a Page's events against Chrome's: the path and order of an event through a document to the window, the
window's error and unhandledrejection events and what then reaches the Session, MutationObserver's records, and the
click of click().

Each page in tests/data/dom_events/ runs its cases and keeps what they give in `window.result`. The `.chrome.json`
beside it is what Chrome gave for the same page, recorded by hand in headless Chrome 157.0.8103.51 on macOS, the Engine
Release's milestone, as tests/data/page_cases.json takes a new case's outcome by hand. A local server serves the pages
at 127.0.0.1 and, as another origin, at localhost, and each test goes through both APIs, with a native library and
the Page program beside it.
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
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

_DATA = Path(__file__).parent / "data" / "dom_events"
# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0
# Waits for the page's own cases, some of which run in later tasks, to keep their result.
_RESULT = """new Promise(resolve => {
  const check = () => window.result ? resolve(window.result) : setTimeout(check, 10);
  check();
})"""


class _DataServer:
    """Serves tests/data/dom_events/ on one port, writing the port where a page says PORT, and takes the POST that a
    page sends with its result, which only Chrome's recording reads."""

    def __init__(self) -> None:
        data_server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                file = _DATA / self.path.lstrip("/").split("?")[0]
                if not file.is_file():
                    self._answer(404, b"", "text/plain")
                    return
                body = file.read_bytes().replace(b"PORT", str(data_server.port).encode())
                kind = "text/html; charset=utf-8" if file.suffix == ".html" else "text/javascript"
                self._answer(200, body, kind)

            def do_POST(self) -> None:
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                self._answer(204, b"", "text/plain")

            def _answer(self, status: int, body: bytes, kind: str) -> None:
                self.send_response(status)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def __enter__(self) -> "_DataServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()

    @property
    def port(self) -> int:
        return self._server.server_port

    def url(self, name: str) -> str:
        return f"http://127.0.0.1:{self.port}/{name}"


@pytest.fixture
def server() -> Iterator[_DataServer]:
    with _DataServer() as data_server:
        yield data_server


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


def _chrome(name: str) -> Any:
    return json.loads((_DATA / f"{name}.chrome.json").read_text())


def _run(session: mitto.Session, server: _DataServer, name: str) -> tuple[Any, list[str]]:
    """Return what the page's cases gave, and the uncaught errors that reached the Session meanwhile."""
    with _open_page(session, server.url(f"{name}.html")) as page:
        return page.evaluate(_RESULT), page.uncaught_errors


def test_events_take_chromes_path_and_order_through_a_document_to_the_window(
    session: mitto.Session, server: _DataServer
) -> None:
    # Capture from the window down, the target's capturing listeners then its others, bubbling back up to the window,
    # stopPropagation(), stopImmediatePropagation(), cancelBubble, returnValue, srcElement, composedPath() at each
    # target, once, a detached tree, passive listeners by default, window.event, a second dispatch, the document's
    # readiness events at the window, load, which stops at the document and has no target once it is done, and an
    # event dispatched at the window after a node, whose composedPath() there keeps the node's path, as in Chrome.
    result, uncaught = _run(session, server, "dispatch")

    assert result == _chrome("dispatch")
    assert uncaught == []


def test_uncaught_errors_and_rejections_reach_the_windows_listeners_and_then_the_session(
    session: mitto.Session, server: _DataServer
) -> None:
    # onerror's five arguments, the ErrorEvent of each place an error can be left uncaught, an inline script's place in
    # its document, "Script error." for a script of another origin, reportError(), unhandledrejection and
    # rejectionhandled, and the constructors.
    result, uncaught = _run(session, server, "errors")

    assert result == _chrome("errors")
    # What a listener canceled stays from the Session, as from Chrome's console, which still hears all of a script of
    # another origin, of an error thrown in an error listener, after the error that the listener ran for, and of a
    # rejection whose unhandledrejection listener handled the promise without canceling the event.
    assert uncaught == [
        "Uncaught TypeError: inline",
        "Uncaught TypeError: Cannot read properties of null (reading 'x')",
        "Uncaught a string",
        "Uncaught URIError: from a file",
        "Uncaught Error: cross-origin secret",
        "Uncaught TypeError: Cannot read properties of null (reading 'inline')",
        "Uncaught RangeError: from a listener",
        "Uncaught Error: reported",
        "Uncaught 42",
        "Uncaught EvalError: from a microtask",
        "Uncaught SyntaxError: from a timer",
        "Uncaught (in promise) Error: rejected",
        "Uncaught (in promise) Error: handle later",
        "Uncaught (in promise) 7",
        "Uncaught (in promise) Error: handled by the listener",
        "Uncaught Error: nest",
        "Uncaught Error: thrown in an error listener",
    ]


def test_mutation_observers_get_chromes_records_in_their_microtask(session: mitto.Session, server: _DataServer) -> None:
    # childList, attributes and characterData records, old values, subtrees, a node that leaves the observed tree,
    # takeRecords(), disconnect(), the order of delivery among microtasks, and observe()'s errors.
    result, uncaught = _run(session, server, "mutations")

    assert result == _chrome("mutations")
    assert uncaught == []


def test_click_fires_chromes_pointer_event_and_dom_activate(session: mitto.Session, server: _DataServer) -> None:
    # click()'s PointerEvent, which no person started, DOMActivate after a click that nothing canceled, a page's own
    # MouseEvent and Event, a click() inside its own click, and a detached element.
    result, uncaught = _run(session, server, "click")

    assert result == _chrome("click")
    assert uncaught == []


def test_a_click_checks_checkboxes_and_radio_buttons_as_chrome_does(
    session: mitto.Session, server: _DataServer
) -> None:
    # The checkedness that a click turns over before the listeners, input and change after them, a canceled click
    # putting back the checkedness, the indeterminate flag and the radio button that was checked, the checked attribute
    # until the checkedness is dirty, a radio button group, a disconnected element, a page's own clicks, a text input
    # and a disabled checkbox.
    result, uncaught = _run(session, server, "activation")

    assert result == _chrome("activation")
    assert uncaught == []
