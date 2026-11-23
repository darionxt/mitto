"""Tests of `new Notification()` in a frame of a Page that a Session opened, on a local server.

The command-line Page program makes no frames, so these open a page through a Session. The expected values are what
headless Chrome for Testing 157 on macOS (157.0.8103.79) gave for the same scripts.
"""

import http.server
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_PAGE = b"<!doctype html><title>t</title><body>"


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of an empty page on 127.0.0.1, which is a secure context."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(_PAGE)))
            self.end_headers()
            self.wfile.write(_PAGE)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_a_notification_of_a_frame_that_goes_throws(page_url: str) -> None:
    # Blink's Notification::Create() throws once its context is destroyed, which it checks after it serializes
    # `data`: here a getter of `data` removes the frame whose constructor runs, and then the frame is gone. The
    # TypeError is the frame's own, so not the parent's TypeError.
    script = """
      (() => {
        const frame = document.createElement('iframe');
        document.body.appendChild(frame);
        const FrameNotification = frame.contentWindow.Notification;
        const outcomes = [];
        for (const data of [{get x() { frame.remove(); return 1; }}, null]) {
          try {
            new FrameNotification('t', {data});
            outcomes.push('constructed');
          } catch (error) {
            outcomes.push([error.name, error.message, error instanceof TypeError]);
          }
        }
        return outcomes;
      })()
    """

    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcomes = page.evaluate(script)

    illegal = ["TypeError", "Failed to construct 'Notification': Illegal invocation.", False]
    assert outcomes == [illegal, illegal]
