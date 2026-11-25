"""Tests of what a response's permissions policy headers give a Page's documents, on a local server.

tests/policy_headers_cases.js runs in a Page at an HTTP origin whose response has a Permissions-Policy header, with a frame for each
form of that header and of Feature-Policy, which gives what its document and its document's frames may do by featurePolicy.
tests/data/policy_headers_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the same scripts, and what the
console said of the headers.
"""

import collections
import http.server
import json
import threading
import urllib.parse
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 180
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "policy_headers_cases.js").read_text()
_CHILD = (_TESTS / "policy_headers_child.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "policy_headers_cases.json").read_text())


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        path, _, query = self.path.partition("?")
        kind, body = "text/html", "<!doctype html><body>"
        headers: list[tuple[str, str]] = []
        if path == "/child.js":
            kind, body = "text/javascript", _CHILD
        elif path in ("/child", "/with"):
            body = f"<!doctype html><script>{_CHILD}</script>"
        if path == "/with":
            asked = json.loads(urllib.parse.unquote(query).replace("LONG", "x" * 66000))
            headers += [("Permissions-Policy", value) for value in asked.get("pp", [])]
            headers += [("Feature-Policy", value) for value in asked.get("fp", [])]
        elif path == "/app/page":
            assert isinstance(self.server, QuietServer)
            port = self.server.server_port
            headers.append(("Permissions-Policy", f'microphone=(), usb=(self "http://localhost:{port}")'))
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body.encode())))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def port() -> Iterator[int]:
    server = QuietServer(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server.server_port
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_what_permissions_policy_headers_give_a_document_is_chrome_s(port: int) -> None:
    url = f"http://127.0.0.1:{port}/app/page"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(message.text for message in page.console_messages)

    assert set(record) == set(_AS_CHROME["record"])
    for headers, expected in _AS_CHROME["record"].items():
        assert record[headers] == expected, headers
    assert console == collections.Counter(_AS_CHROME["console"])
