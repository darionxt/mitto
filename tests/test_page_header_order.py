"""Tests of the order on the wire of the header lines that a page's script sets on fetch().

tests/header_order_cases.js runs in a Page at an HTTP origin. tests/data/header_order_cases.json holds the names of the
lines that Chrome 157 sent for each of its requests, in their order, headless Chrome for Testing on macOS
(157.0.8103.79), whose fetches have the lines of Windows Chrome's, in the same order. They leave out Range, which the
HTTP cache, which Chrome has and a Session has only if asked, takes to the end of a GET, and the lines of conditional
requests.
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
_PAGE = b"<!doctype html><title>t</title>"
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "header_order_cases.js").read_text()
_AS_CHROME: dict[str, list[str]] = json.loads((_TESTS / "data" / "header_order_cases.json").read_text())


class HeaderOrderServer(QuietServer):
    """The server that tests/header_order_cases.js runs on, which keeps the names of each request's lines by its path."""

    def __init__(self) -> None:
        self.names: dict[str, list[str]] = {}
        super().__init__(("127.0.0.1", 0), _Handler)


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _handle(self) -> None:
        assert isinstance(self.server, HeaderOrderServer)
        self.server.names[self.path.removeprefix("/")] = self.headers.keys()
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(_PAGE)))
        self.end_headers()
        self.wfile.write(_PAGE)

    def do_GET(self) -> None:
        self._handle()

    def do_POST(self) -> None:
        self._handle()

    def do_PUT(self) -> None:
        self._handle()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server() -> Iterator[HeaderOrderServer]:
    server = HeaderOrderServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_a_scripts_header_lines_go_in_the_order_of_blinks_header_map(server: HeaderOrderServer) -> None:
    url = f"http://127.0.0.1:{server.server_port}/"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS) == "done"

    for name, expected in _AS_CHROME.items():
        assert server.names[name] == expected, name
