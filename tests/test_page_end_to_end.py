"""The Runtime skeleton end to end, through Python: a Session with a cookie opens a Page on a local server, whose script
fetches JSON with that cookie and writes it into the document. Then the Page's process is killed, and the Session goes on.

The test goes through both APIs, and needs a native library with the Page program beside it, as CI's native job installs
it on every Host. Without the program it skips.
"""

import http.server
import json
import os
import signal
import sys
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._errors import PageEndedError
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_PAGE = (
    "<p id=out>waiting</p>"
    "<script>window.done = fetch('/data.json').then(response => response.json()).then(data => {"
    " document.getElementById('out').textContent = JSON.stringify(data); return data; });</script>"
)


class _Handler(http.server.BaseHTTPRequestHandler):
    """Serves the page at / and, at /data.json, JSON that holds the Cookie line the request carried."""

    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        if self.path == "/":
            body, content_type = _PAGE.encode(), "text/html; charset=utf-8"
        elif self.path == "/data.json":
            body, content_type = json.dumps({"cookie": self.headers.get("Cookie")}).encode(), "application/json"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server_url() -> Iterator[str]:
    with QuietServer(("127.0.0.1", 0), _Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        yield f"http://127.0.0.1:{server.server_port}"
        server.shutdown()


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


def test_a_page_fetches_json_with_its_sessions_cookie_into_its_document_and_its_session_outlives_its_process(
    session: mitto.Session, server_url: str
) -> None:
    session.cookies.set(mitto.Cookie("token", "m6", domain="127.0.0.1"))
    sent = {"cookie": "token=m6"}

    with _open_page(session, f"{server_url}/") as page:
        assert page.evaluate("done", timeout=_TIMEOUT_SECONDS) == sent
        assert json.loads(page.evaluate("document.getElementById('out').textContent")) == sent

        os.kill(page.process_id, signal.SIGTERM if sys.platform == "win32" else signal.SIGKILL)

        with pytest.raises(PageEndedError) as raised:
            page.evaluate("1")
        assert raised.value.message.startswith("the Page process ")
        with pytest.raises(PageEndedError) as again:
            page.evaluate("1")
        assert again.value.message == raised.value.message

    response = session.get(f"{server_url}/data.json")
    assert response.status == 200
    assert response.json() == sent
