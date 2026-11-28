"""Tests of a Page's XMLHttpRequest, on a local server.

tests/xhr_cases.js runs in a Page at an HTTP origin and gives what asynchronous and synchronous requests did there, in
the window and in a worker. tests/data/xhr_cases.json holds what Chrome 157 gave for the same script on the same server,
headless Chrome for Testing on macOS (157.0.8103.79), with the page's origin as HERE and the same server's name
"localhost" as CROSS. It ran with RequiresLoadIgnoreLimitsForDomWindowsOnly off, as it is by default and for most stable
Windows installations by the seed of 2026-11-25, where Chrome for Testing has it on.
"""

import collections
import gzip
import http.server
import json
import threading
import time
import urllib.parse
import zlib
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_PAGE = b"<!doctype html><title>t</title><body>"
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "xhr_cases.js").read_text(encoding="utf-8")
_AS_CHROME = json.loads((_TESTS / "data" / "xhr_cases.json").read_text(encoding="utf-8"))
# The request headers that /seen gives back.
_SEEN = (
    *("accept", "authorization", "content-length", "content-type", "origin", "referer", "x-a", "x-b"),
    *("sec-fetch-dest", "sec-fetch-mode", "sec-fetch-site"),
)


class XhrServer(QuietServer):
    """The server that tests/xhr_cases.js runs on."""

    def __init__(self) -> None:
        self.seen: dict[str, dict[str, Any]] = {}
        self.counts = collections.Counter[str]()
        self.released = collections.defaultdict[str, threading.Event](threading.Event)
        self.stopping = threading.Event()
        super().__init__(("127.0.0.1", 0), _Handler)


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _server(self) -> XhrServer:
        assert isinstance(self.server, XhrServer)
        return self.server

    def _answer(
        self, body: bytes, *lines: tuple[str, str], status: int = 200, content_type: str = "text/plain"
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        for name, value in lines:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self) -> None:
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        _, route, *rest = self.path.split("/")
        name = rest[0] if rest else ""
        match route:
            case "bytes":
                self._answer(b"a" * int(name))
            case "late":
                # As /bytes, 150 ms later: past the 50 ms of Blink's progress throttle, whichever host runs it.
                time.sleep(0.15)
                self._answer(b"a" * int(name))
            case "echo":
                self._answer(body)
            case "log":
                headers = {key: value for key in _SEEN if (value := self.headers.get(key)) is not None}
                self._server().seen[name] = {"method": self.command, "headers": headers, "body": body.decode("latin-1")}
                self._answer(b"logged")
            case "seen":
                self._answer(json.dumps(self._server().seen[name]).encode(), content_type="application/json")
            case "status":
                self._answer(b"xyz", status=int(name))
            case "redirect":
                self._answer(b"", ("Location", "/bytes/3"), status=302)
            case "to":
                lines = [("Access-Control-Allow-Origin", "*"), ("Location", urllib.parse.unquote(name))]
                self._answer(b"", *lines, status=302)
            case "twice":
                self._answer(b"x", ("X-Twice", "1"), ("x-twice", "2"))
            case "cors":
                lines = [("Access-Control-Allow-Origin", "*"), ("X-Hidden", "1"), ("X-Shown", "2")]
                self._answer(b"ok", *lines, ("Access-Control-Expose-Headers", "x-shown"))
            case "expose":
                lines = [("Access-Control-Allow-Origin", "*"), ("X-Shown", "1"), ("X-Hidden", "2")]
                self._answer(b"ok", *lines, ("Access-Control-Expose-Headers", urllib.parse.unquote(name)))
            case "encoded":
                encodings = urllib.parse.unquote(name)
                body = b"a" * 1000
                if encodings.lower().removeprefix("x-") == "gzip":
                    body = gzip.compress(body, mtime=0)
                elif encodings == "deflate":
                    body = zlib.compress(body)
                self._answer(body, ("Content-Encoding", encodings))
            case "pieces":
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", "2000")
                self.end_headers()
                for piece in (b"a" * 10, b"b" * 5):
                    self.wfile.write(piece)
                    self.wfile.flush()
                    time.sleep(0.02)
                self._server().stopping.wait(_TIMEOUT_SECONDS)
                self.close_connection = True
            case "held":
                # The first bytes at once, and the rest once /release/ID has come.
                kind, options = urllib.parse.unquote(rest[1]).split("|")
                first, last = (b"a" * 2000, b"b") if "long" in options else (b"a" * 10, b"b" * 5)
                self.send_response(200)
                for content_type in filter(None, kind.split("+")):
                    self.send_header("Content-Type", content_type)
                if "nosniff" in options:
                    self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Content-Length", str(len(first) + len(last)))
                self.end_headers()
                self.wfile.write(first)
                self.wfile.flush()
                self._server().released[name].wait(_TIMEOUT_SECONDS)
                self.wfile.write(last)
            case "release":
                self._server().released[name].set()
                self._answer(b"released")
            case "lengths":
                self._answer(b"a" * 5, *[("Content-Length", "5")] * (int(name) - 1))
            case "empty":
                self._answer(b"")
            case "slow":
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", "30")
                self.end_headers()
                self.wfile.write(b"a" * 10)
                self.wfile.flush()
                time.sleep(0.2)
                self.wfile.write(b"a" * 20)
            case "count":
                self._server().counts[name] += 1
                count = self._server().counts[name]
                if name.startswith("slow"):
                    time.sleep(0.2)
                lines = [("Access-Control-Allow-Origin", "*"), ("X-Count", str(count)), ("Cache-Control", "no-store")]
                self._answer(str(count).encode(), *lines)
            case "never":
                self._server().stopping.wait(_TIMEOUT_SECONDS)
                self.close_connection = True
            case "with":
                # A response of the type `name`, with the lines, each "name: value" and joined by "|", and the body that
                # the next two parts give, or the page for no body.
                lines = [
                    (key, value)
                    for key, _, value in (line.partition(": ") for line in urllib.parse.unquote(rest[1]).split("|"))
                    if key
                ]
                answer = urllib.parse.unquote(rest[2]).encode() or _PAGE
                self._answer(answer, *lines, content_type=urllib.parse.unquote(name))
            case _:
                self._answer(_PAGE, content_type="text/html")

    def do_GET(self) -> None:
        self._handle()

    def do_HEAD(self) -> None:
        self._handle()

    def do_POST(self) -> None:
        self._handle()

    def do_PUT(self) -> None:
        self._handle()

    def do_DELETE(self) -> None:
        self._handle()

    def do_patch(self) -> None:
        self._handle()

    def do_Custom(self) -> None:
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
def page_url() -> Iterator[str]:
    """The URL of an empty page on a server for tests/xhr_cases.js."""
    server = XhrServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.stopping.set()
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_xml_http_requests_do_what_chrome_s_do(page_url: str) -> None:
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_a_request_too_large_for_a_message_fails_once_send_has_returned(page_url: str) -> None:
    script = """new Promise((resolve) => {
      const log = [];
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/echo');
      xhr.send(new Uint8Array(17 << 20));
      log.push(`sent ${xhr.readyState}`);
      for (const type of ['error', 'loadend']) xhr.addEventListener(type, () => log.push(`${type} ${xhr.status}`));
      xhr.addEventListener('loadend', () => resolve(log));
    })"""
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == ["sent 1", "error 0", "loadend 0"]
