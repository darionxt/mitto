"""Tests of which frames a Page's documents may navigate, and what a navigation that a frame starts sends, on a local server.

tests/frames_targets_cases.js makes a page's frames and has one of them, the actor, or the page itself, navigate the page or another
frame by location, window.open() or a link. Each document logs to the server what its call gave and which frame moved, and the server what
the request said. tests/data/frames_targets_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the same scripts,
with the console.
"""

import collections
import http.server
import json
import re
import threading
import time
import urllib.parse
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_targets_cases.js").read_text()
_AS_CHROME: dict[str, Any] = json.loads((_TESTS / "data" / "frames_targets_cases.json").read_text())["record"]
_HEADERS = ("Referer", "Origin", "Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest", "Sec-Fetch-User")
# How long an act has to show that nothing more comes of it once all that Chrome gave has come.
_GRACE_SECONDS = 0.1


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


class _Server(QuietServer):
    """Serves the cases of one actor, and keeps what their documents say of each act."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        self.finished = threading.Event()
        self.expected: list[dict[str, Any]] = []
        self.acts: dict[int, dict[str, Any]] = {}
        # The events of each act, with their place among what their document logged.
        self._events: dict[int, list[tuple[int, str]]] = {}
        self._lock = threading.Condition()

    def plain(self, text: str) -> str:
        """Return `text` without what changes from run to run: the port, and the queries of the cases' URLs."""
        origins = text.replace(f"http://127.0.0.1:{self.server_port}", "A")
        origins = origins.replace(f"http://localhost:{self.server_port}", "B")
        origins = origins.replace(f"http%3A%2F%2F127.0.0.1%3A{self.server_port}", "A")
        return re.sub(r"(/(?:moved|frame|top|nest))[?][^' ]*", r"\1", origins)

    def add(self, at: int, kind: str, value: str, order: int = 0) -> None:
        with self._lock:
            act = self.acts.setdefault(at, {"result": None, "moved": [], "requests": [], "navigate": []})
            if kind == "result":
                act["result"] = value
            elif kind == "navigate":
                events = self._events.setdefault(at, [])
                events.append((order, value))
                act["navigate"] = [event for _, event in sorted(events)]
            else:
                act[kind].append(value)
            self._lock.notify_all()

    def wait_for(self, at: int, *, result_only: bool) -> None:
        """Wait until act `at` has given its result, and unless `result_only` all that it gave in Chrome, and a little more."""
        expected = self.expected[at]

        def count(act: dict[str, Any], kind: str) -> int:
            # How many entries the history has gained is told after the wait.
            return sum(not value.endswith("more entries") for value in act[kind])

        def done() -> bool:
            act = self.acts.get(at)
            if act is None or act["result"] is None:
                return False
            return result_only or all(
                count(act, kind) >= count(expected, kind) for kind in ("moved", "requests", "navigate")
            )

        with self._lock:
            self._lock.wait_for(done, timeout=10)
        if not result_only:
            time.sleep(_GRACE_SECONDS)


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        assert isinstance(self.server, _Server)
        path, _, query = self.path.partition("?")
        kind, body = "text/html", "<!doctype html><body><script src='/cases.js'></script><script>run()</script>"
        if path == "/cases.js":
            kind, body = "text/javascript", _CASES
        elif path == "/plain":
            body = "<!doctype html><body>"
        elif path in ("/settle", "/moved"):
            at = int(urllib.parse.parse_qs(query)["at"][0])
            self.server.wait_for(at, result_only=path == "/moved")
            if path == "/moved":
                host = "A" if self.headers["Host"].startswith("127.") else "B"
                said = [re.sub(r"[?].*", "", self.server.plain(self.headers.get(name, "-"))) for name in _HEADERS]
                self.server.add(at, "requests", " ".join([host, *said]))
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body.encode())))
        self.end_headers()
        self.wfile.write(body.encode())

    def do_POST(self) -> None:
        assert isinstance(self.server, _Server)
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/finished":
            self.server.finished.set()
        else:
            entry = json.loads(body)
            if "result" in entry:
                self.server.add(entry["at"], "result", entry["result"])
            elif "moved" in entry:
                moved = " ".join((entry["moved"], entry["referrer"] or "-", entry["opener"]))
                self.server.add(entry["at"], "moved", moved)
            else:
                self.server.add(entry["at"], "navigate", entry["navigate"], entry["order"])
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[_Server]:
    server = _Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("actor", _AS_CHROME)
def test_what_a_frame_navigates_is_chrome_s(server: _Server, actor: str) -> None:
    expected = _AS_CHROME[actor]
    server.expected = expected["acts"]
    url = f"http://127.0.0.1:{server.server_port}/top?actor={urllib.parse.quote(actor)}&at=0"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while not server.finished.wait(0.05) and time.monotonic() < deadline:
            # Runs the page's events, whichever document it has.
            page.evaluate("1", timeout=_TIMEOUT_SECONDS)
        # What came since the last time.
        page.evaluate("1", timeout=_TIMEOUT_SECONDS)
        console = collections.Counter(server.plain(message.text).strip() for message in page.console_messages)

    assert server.finished.is_set()
    assert sorted(server.acts) == list(range(len(expected["acts"])))
    for at, act in enumerate(expected["acts"]):
        assert server.acts[at] == act, f"{actor}: {at}"
    assert console == collections.Counter(expected["console"])
