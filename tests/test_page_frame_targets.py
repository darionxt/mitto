"""Tests of which frames a Page's documents may navigate, and what a navigation that a frame starts sends, on a local server.

tests/frames_targets_cases.js makes a page's frames and has one of them, the actor, or the page itself, navigate the page or another
frame by location, window.open() or a link. Each document logs to the server what its call gave and which frame moved, and the server what
the request said. tests/data/frames_targets_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the same scripts,
with the console.
"""

import collections
import contextlib
import http.server
import json
import re
import threading
import time
import urllib.parse
from collections.abc import Generator, Iterator
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
        """Return `text` without what changes from run to run: the port, the queries of the cases' URLs, and the names of blob: URLs."""
        origins = text.replace(f"http://127.0.0.1:{self.server_port}", "A")
        origins = origins.replace(f"http://localhost:{self.server_port}", "B")
        origins = origins.replace(f"http%3A%2F%2F127.0.0.1%3A{self.server_port}", "A")
        origins = re.sub(r"(blob:null/)[0-9a-f-]{36}", r"\1UUID", origins)
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


class _Pages(QuietServer):
    """Serves `pages` by path, with their own origins for A and B, and keeps the paths asked for."""

    def __init__(self, pages: dict[str, str]) -> None:
        super().__init__(("127.0.0.1", 0), _PagesHandler)
        self.paths: list[str] = []
        origins = {"A": f"http://127.0.0.1:{self.server_port}", "B": f"http://localhost:{self.server_port}"}
        self.url = origins["A"]
        self.pages = {
            path: re.sub(r"\b[AB](?=/)", lambda match: origins[match[0]], body) for path, body in pages.items()
        }


class _PagesHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        assert isinstance(self.server, _Pages)
        if self.path != "/favicon.ico":
            self.server.paths.append(self.path)
        body = ("<!doctype html>" + self.server.pages.get(self.path.partition("?")[0], "")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@contextlib.contextmanager
def _page_of(pages: dict[str, str], path: str) -> Generator[tuple[Any, _Pages]]:
    """Open `path` of `pages` and give its Page and the server."""
    server = _Pages(pages)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with mitto.Session() as session, session._open_page(server.url + path, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
            yield page, server
    finally:
        server.shutdown()
        server.server_close()


def _until(page: Any, condition: str) -> None:
    """Wait until `condition` holds in the document that `page` has."""
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while not page.evaluate(condition, timeout=_TIMEOUT_SECONDS):
        assert time.monotonic() < deadline, condition
        time.sleep(0.02)


@pytest.mark.usefixtures("page_program")
def test_a_link_in_a_frameset_to_its_own_fragment_loads_it_again() -> None:
    pages = {
        "/set": "<frameset><frame name='inner' src='/inner'></frameset>",
        "/inner": "<body><a target='_top' href='/set#y'></a>",
    }
    with _page_of(pages, "/set") as (page, server):
        _until(page, "document.readyState === 'complete'")
        page.evaluate("window.first = true; frames.inner.document.querySelector('a').click()", timeout=_TIMEOUT_SECONDS)
        _until(page, "!window.first && document.readyState === 'complete' && location.hash === '#y'")
        assert server.paths == ["/set", "/inner", "/set", "/inner"]


@pytest.mark.usefixtures("page_program")
def test_a_fragment_of_another_process_starts_again_where_the_url_has_changed() -> None:
    pages = {
        "/race": """<body><script>
            const {port1, port2} = new MessageChannel();
            port1.onmessage = () => history.pushState(null, '', '/elsewhere');
            onmessage = () => {
              if (location.hash) return;
              postMessage.call(frames[0], 'go', '*');
              port2.postMessage('push');
            };
        </script><iframe src='B/frame'></iframe>""",
        "/frame": "<body><script>onmessage = () => open('A/race#x', '_top'); postMessage.call(parent, 'ready', '*');</script>",
    }
    with _page_of(pages, "/race") as (page, server):
        _until(page, "location.hash === '#x' && document.readyState === 'complete'")
        assert page.evaluate("location.pathname", timeout=_TIMEOUT_SECONDS) == "/race"
        assert server.paths == ["/race", "/frame", "/race", "/frame"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_of_a_document_of_another_origin_is_chrome_s_error_page() -> None:
    pages = {
        "/top": "<body><script>said = []; onmessage = ({data}) => said.push(data);</script><iframe src='B/mid'></iframe>",
        "/mid": """<body><script>
            window.tell = (data) => postMessage.call(parent, data, '*');
        </script><iframe name='deep' srcdoc='<script>parent.tell(`ran`)</script>'
            onload="tell(`load ${this.contentDocument?.URL}`)"></iframe>""",
    }
    # As in Chrome 157: a fragment is within the document, whoever starts it, and another srcdoc only its parent's origin's.
    with _page_of(pages, "/top") as (page, _):
        _until(page, "said.length === 2")
        for count, url in enumerate(("about:srcdoc#x", "about:srcdoc"), 3):
            page.evaluate(f"open('{url}', 'deep'); 0", timeout=_TIMEOUT_SECONDS)
            _until(page, f"said.length === {count}")
        said = page.evaluate("said", timeout=_TIMEOUT_SECONDS)
    assert said == ["ran", "load about:srcdoc", "load about:srcdoc#x", "load undefined"]


@pytest.mark.usefixtures("page_program")
def test_a_page_keeps_itself_as_its_opener_until_a_navigation_adds_an_entry() -> None:
    with _page_of({}, "/own") as (page, _):
        page.evaluate("name = 'roof'; open('', 'roof'); 0", timeout=_TIMEOUT_SECONDS)
        for navigation, kept in (
            ("location.reload()", True),
            ("location.href = location.href", True),
            ("location.replace('/own?y')", True),
            ("location.href = '/own?x'", False),
        ):
            assert page.evaluate("opener === window", timeout=_TIMEOUT_SECONDS)
            page.evaluate(f"window.before = true; {navigation}", timeout=_TIMEOUT_SECONDS)
            _until(page, "!window.before && document.readyState === 'complete'")
            assert page.evaluate("opener === window", timeout=_TIMEOUT_SECONDS) is kept
