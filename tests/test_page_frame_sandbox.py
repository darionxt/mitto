"""Tests of what an iframe's sandbox withholds from its documents in a Page, on a local server.

tests/frames_sandbox_cases.js runs in a Page at an HTTP origin with sandboxed frames of each kind of document, and gives what the
page reaches of each, what each says of itself and may do, when the attribute holds, what frames within a sandbox have, and what
runs in a document without scripts. tests/data/frames_sandbox_cases.json holds what headless Chrome for Testing 157.0.8103.79
gave for the same scripts, what the server got from the frames, and what the console said of sandboxes.
"""

import collections
import http.server
import json
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_sandbox_cases.js").read_text()
_CHILD = (_TESTS / "frames_sandbox_child.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_sandbox_cases.json").read_text())
# Says whether it has times of the document before it, once it has navigated, which it does itself for ?itself.
_TIMES = """<!doctype html><script>
if (location.search === '?itself') onload = () => setTimeout(() => location.replace('?then'));
else if (location.search) {
  const [entry] = performance.getEntriesByType('navigation');
  const said = [performance.timing.unloadEventStart > 0, performance.timing.unloadEventEnd > 0, entry.unloadEventEnd > 0];
  postMessage.call(parent, said, '*');
}
</script>"""
_HEADERS = {
    "origin",
    "cookie",
    "referer",
    "sec-fetch-site",
    "sec-fetch-mode",
    "sec-fetch-dest",
    "sec-fetch-storage-access",
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


class _Server(QuietServer):
    """Answers the page, its frames and what they ask for, and keeps the requests of the frames, in the order they came."""

    def __init__(self) -> None:
        self.seen: list[tuple[str, dict[str, str]]] = []
        super().__init__(("127.0.0.1", 0), _Handler)


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        assert isinstance(self.server, _Server)
        path = self.path
        if path.startswith(("/seen", "/cors", "/child?", "/worker.js", "/ran.js")):
            headers = {name.lower(): value for name, value in self.headers.items() if name.lower() in _HEADERS}
            self.server.seen.append((path, headers))
        if path.startswith("/empty?slow"):
            time.sleep(0.3)
        if path == "/closed":
            self.close_connection = True
            return
        kind, body, more = "text/html", "<!doctype html><title>t</title><body>", dict[str, str]()
        if path.startswith("/app"):
            more = {"Set-Cookie": "top=1; Path=/"}
        elif path == "/child.js":
            kind, body = "text/javascript", _CHILD
        elif path.startswith("/child?"):
            body = f"<!doctype html><body><script>{_CHILD}</script>"
        elif path == "/hear":
            body = "<!doctype html><script>onmessage = (e) => postMessage.call(parent, 'heard ' + e.data, '*')</script>"
        elif path == "/secure":
            body = "<script>postMessage.call(top, ['inner', location.protocol, isSecureContext], '*')</script>"
        elif path.startswith("/times"):
            body = _TIMES
        elif path == "/refresh":
            body = "<!doctype html><meta http-equiv=refresh content='0; url=/empty?refreshed'>"
        elif path == "/refresh-header":
            more = {"Refresh": "0; url=/empty?refreshed"}
        elif path.startswith("/worker.js"):
            kind, body = "text/javascript", "postMessage(1)"
        elif path == "/ran.js":
            kind, body = "text/javascript", "parent.ran.push('external')"
        elif path.startswith("/seen.svg"):
            kind, body = "image/svg+xml", "<svg xmlns='http://www.w3.org/2000/svg'/>"
        elif path.startswith("/seen"):
            kind, body = "text/javascript", "1"
        elif path.startswith("/cors"):
            kind, body = "text/javascript", "1"
            more = {
                "Access-Control-Allow-Origin": self.headers.get("Origin", "*"),
                "Access-Control-Allow-Credentials": "true",
            }
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        for name, value in more.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[_Server]:
    server = _Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server
    server.shutdown()
    server.server_close()


def _plain(text: str, port: int) -> str:
    origin = f"http://127.0.0.1:{port}"
    return (
        text.replace(f"{origin}/app/page", "PAGE")
        .replace(origin, "ORIGIN")
        .replace(f"http://localhost:{port}", "OTHER")
    )


@pytest.mark.usefixtures("page_program")
def test_what_a_sandbox_withholds_is_chrome_s(server: _Server) -> None:
    port = server.server_port
    url = f"http://127.0.0.1:{port}/app/page"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(message.text for message in page.console_messages if "sandbox" in message.text)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section
    assert console == collections.Counter(_AS_CHROME["console"])

    # What the server got, by the URL, which names the document that asked.
    requests: dict[str, list[dict[str, str]]] = {}
    for path, headers in server.seen:
        sent = {name: _plain(value, port) for name, value in sorted(headers.items())}
        if sent not in requests.setdefault(path, []):
            requests[path].append(sent)
    assert requests == _AS_CHROME["requests"]
