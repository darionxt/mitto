"""Tests of what a Page's calls that a permissions policy or a frame's ancestors decide give in its frames, on a local server.

tests/frames_apis_cases.js runs in a Page at an HTTP origin of a secure context, with a frame for each kind of origin, sandbox, allow and
header, each of which answers what it reads of the permissions and what its calls give: the page's own, and only what differs from that
for a frame. tests/data/frames_apis_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the same scripts, and what
the console said of the policy.
"""

import collections
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

_TIMEOUT_SECONDS = 180
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_apis_cases.js").read_text()
_CHILD = (_TESTS / "frames_apis_child.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_apis_cases.json").read_text())
# What the header of /without and /app/without takes from its document.
_WITHHELD = (
    "accelerometer",
    "camera",
    "captured-surface-control",
    "ch-ua-high-entropy-values",
    "clipboard-read",
    "clipboard-write",
    "digital-credentials-get",
    "display-capture",
    "encrypted-media",
    "fullscreen",
    "gamepad",
    "geolocation",
    "gyroscope",
    "identity-credentials-get",
    "idle-detection",
    "keyboard-map",
    "local-fonts",
    "local-network-access",
    "magnetometer",
    "microphone",
    "midi",
    "otp-credentials",
    "payment",
    "publickey-credentials-get",
    "screen-wake-lock",
    "speaker-selection",
    "storage-access",
    "web-app-installation",
    "window-management",
)
# The headers of the paths that have one of their own.
_HEADERS = {
    "/self": "ch-ua-high-entropy-values=(self)",
    "/neither": "local-network=(), loopback-network=()",
    "/one": "local-network=()",
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


class _Server(QuietServer):
    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        # Set once the script of the cases wants /late answered.
        self.release = threading.Event()


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        path = self.path.partition("?")[0]
        kind, body = "text/html", "<!doctype html><body>"
        if path == "/child.js":
            kind, body = "text/javascript", _CHILD
        elif path in ("/child", "/nest", "/without", "/late", *_HEADERS):
            body = f"<!doctype html><script>{_CHILD}</script>"
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body.encode())))
        assert isinstance(self.server, _Server)
        if path == "/late":
            self.server.release.wait(_TIMEOUT_SECONDS)
        elif path == "/release":
            self.server.release.set()
        if path in ("/without", "/late", "/app/without"):
            self.send_header("Permissions-Policy", ", ".join(f"{feature}=()" for feature in _WITHHELD))
        elif path in _HEADERS:
            self.send_header("Permissions-Policy", _HEADERS[path])
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def port() -> Iterator[int]:
    server = _Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server.server_port
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_what_a_pages_calls_give_in_its_frames_is_chrome_s(port: int) -> None:
    url = f"http://127.0.0.1:{port}/app/page"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate(_CHILD, timeout=_TIMEOUT_SECONDS)
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(message.text for message in page.console_messages)

    assert {*record, "top without"} == set(_AS_CHROME["record"])
    for frame, answers in record.items():
        assert answers == _AS_CHROME["record"][frame], frame
    assert console == collections.Counter(_AS_CHROME["console"])


@pytest.mark.usefixtures("page_program")
def test_what_a_pages_calls_give_under_its_own_header_is_chrome_s(port: int) -> None:
    url = f"http://127.0.0.1:{port}/app/without"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate(_CHILD, timeout=_TIMEOUT_SECONDS)
        state = page.evaluate("apis('state')", timeout=_TIMEOUT_SECONDS)
        calls = page.evaluate("apis('calls')", timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(message.text for message in page.console_messages)

    assert {"state": state, "calls": calls} == _AS_CHROME["record"]["top without"]
    assert console == collections.Counter(_AS_CHROME["console without"])
