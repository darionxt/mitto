"""Tests of what a frame's documents inherit of a Page's permissions policy, on a local server.

tests/frames_policy_cases.js runs in a Page at an HTTP origin with frames of each kind of document and of each allow attribute, and
gives what their documents' and their elements' featurePolicy say. tests/data/frames_policy_cases.json holds what headless Chrome
for Testing 157.0.8103.79 gave for the same scripts, and what the console said of policies.
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

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_policy_cases.js").read_text()
_CHILD = (_TESTS / "frames_policy_child.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_policy_cases.json").read_text())
# A document with a frame of the origin that its query names, which passes on what the frame says.
_HOLDS = """<!doctype html><body><script>
onmessage = (event) => postMessage.call(parent, event.data, '*');
const frame = document.createElement('iframe');
frame.allow = 'camera *; geolocation';
frame.src = `${decodeURIComponent(location.search.slice(1))}/child`;
document.body.append(frame);
</script>"""


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
        status, kind, body, location = 200, "text/html", "<!doctype html><body>", ""
        if path == "/child.js":
            kind, body = "text/javascript", _CHILD
        elif path == "/child":
            body = f"<!doctype html><script>{_CHILD}</script>"
        elif path == "/goes":
            body = f"<!doctype html><script>location.replace('{query}')</script>"
        elif path == "/holds":
            body = _HOLDS
        elif path == "/redirect":
            status, location = 302, query
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body.encode())))
        if location:
            self.send_header("Location", location)
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
def test_what_a_frame_inherits_of_a_permissions_policy_is_chrome_s(port: int) -> None:
    url = f"http://127.0.0.1:{port}/app/page"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(message.text for message in page.console_messages)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section
    assert console == collections.Counter(_AS_CHROME["console"])
