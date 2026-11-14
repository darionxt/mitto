"""Tests of what the library works out for a Page's frames and dedicated workers, from what the Page tells it of each.

The Page program makes no frame or worker yet, so a fake Page program stands in for it: a Python script beside a copy of
the library, which speaks the messages of native/page_messages.h. It commits a frame of another site under the top-level
document, which fetches, and starts a worker, which fetches too. A Session in another process opens the Page, and two
local servers, on hosts of two sites, record what each request carried.
"""

import http.server
import os
import shutil
import subprocess
import sys
import textwrap
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

pytestmark = [
    pytest.mark.usefixtures("native_library"),
    pytest.mark.skipif(sys.platform == "win32", reason="the fake Page program is a script"),
]

_TIMEOUT_SECONDS = 60

# The version of the messages of native/page_messages.h.
_PROTOCOL_VERSION = 20

_FAKE_PAGE = textwrap.dedent(
    """\
    #!{python}
    # Stands in for the Page program: a top-level document, a frame of another site in it, and a worker.
    import socket
    import struct
    import sys

    channel = socket.socket(fileno=int(sys.argv[1].removeprefix("--session=").split(",")[0]))
    pending = b""

    def send(message_type, payload):
        channel.sendall(struct.pack("<II", len(payload), message_type) + payload)

    def receive():
        global pending
        while len(pending) < 8 or len(pending) < 8 + struct.unpack_from("<I", pending)[0]:
            chunk = channel.recv(65536)
            if not chunk:
                sys.exit(0)
            pending += chunk
        size, message_type = struct.unpack_from("<II", pending)
        payload = pending[8 : 8 + size]
        pending = pending[8 + size :]
        return message_type, payload

    def sized(data):
        return struct.pack("<I", len(data)) + data

    def request(document, number, kind, url, target=0):
        return (
            struct.pack("<QQBB", document, number, kind, 0)
            + struct.pack("<BBBBBQ", 0, 0, 0, 0, 0, target)
            + sized(url.encode()) + sized(b"GET") + sized(b"") + sized(b"") + b"\\x00"
        )

    def wait_for_end(number):
        while True:
            message_type, payload = receive()
            if message_type == 15 and struct.unpack_from("<Q", payload)[0] == number:
                return

    send(1, struct.pack("<I", {version}))
    send(70, struct.pack("<QBQ", 1, 0, 0))
    wait_for_end(0)
    send(70, struct.pack("<QBQQ", 2, 1, 0, 0))
    send(12, struct.pack("<Q", 2))
    while True:
        message_type, payload = receive()
        if message_type == 4:
            call = struct.unpack_from("<Q", payload)[0]
            break
    # The frame's initial about:blank, then its navigation, of Kind iframe, whose document takes its place.
    send(70, struct.pack("<QBQ", 3, 0, 2))
    send(8, request(2, 1, 11, "{frame}", target=3))
    wait_for_end(1)
    send(70, struct.pack("<QBQQ", 4, 1, 2, 1))
    send(8, request(4, 2, 2, "{frame_fetch}"))
    wait_for_end(2)
    # A worker of the top-level document, from its script's response, which fetches.
    send(8, request(2, 3, 9, "{worker}"))
    wait_for_end(3)
    send(100, struct.pack("<QQBBQ", 5, 2, 1, 0, 3))
    send(8, request(5, 4, 2, "{worker_fetch}"))
    wait_for_end(4)
    send(5, struct.pack("<QB", call, 0) + b"null")
    while channel.recv(65536):
        pass
    """
)

# Opens a Page at the URL in argv[1] in a Session that has the frame site's cookie, and asks it for something.
_SESSION = textwrap.dedent(
    """\
    import sys

    import mitto

    with mitto.Session() as session:
        session.get(sys.argv[2])
        with session._open_page(sys.argv[1], timeout=60) as page:
            page.evaluate("contexts", timeout=60)
    """
)


class _Server:
    """A local server that answers each path with a small text and records the header lines of each request."""

    def __init__(self, host: str) -> None:
        self.host = host
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.lock = threading.Lock()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with server.lock:
                    server.requests.append((self.path, list(self.headers.items())))
                body = b"<p>page</p>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html" if self.path.endswith("page") else "text/javascript")
                if self.path == "/set":
                    self.send_header("Set-Cookie", "f=1")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str) -> str:
        return f"http://{self.host}:{self._server.server_port}{path}"

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        with self.lock:
            return [headers for requested, headers in self.requests if requested == path]

    def __enter__(self) -> "_Server":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def top() -> Iterator[_Server]:
    with _Server("127.0.0.1") as server:
        yield server


@pytest.fixture
def frame_site() -> Iterator[_Server]:
    # localhost is another site than 127.0.0.1, and resolves to it.
    with _Server("localhost") as server:
        yield server


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


def _run_page(tmp_path: Path, top: _Server, frame_site: _Server) -> None:
    library = _native.find_library()
    assert library is not None
    directory = tmp_path / "library"
    directory.mkdir()
    copy = directory / library.name
    shutil.copy2(library, copy)
    page_program = directory / _native.PAGE_PROGRAM_NAME
    page_program.write_text(
        _FAKE_PAGE.format(
            python=sys.executable,
            version=_PROTOCOL_VERSION,
            frame=frame_site.url("/frame-page"),
            frame_fetch=frame_site.url("/frame-fetch"),
            worker=top.url("/worker.js"),
            worker_fetch=top.url("/worker-fetch"),
        )
    )
    page_program.chmod(0o755)
    environment = {**os.environ, _native.LIBRARY_ENV_VAR: str(copy)}
    subprocess.run(
        [sys.executable, "-c", _SESSION, top.url("/page"), frame_site.url("/set")],
        env=environment,
        check=True,
        timeout=_TIMEOUT_SECONDS,
    )


def test_a_frames_requests_have_its_place_in_the_frame_tree(tmp_path: Path, top: _Server, frame_site: _Server) -> None:
    _run_page(tmp_path, top, frame_site)

    # The frame's navigation goes as a frame of another site's does, from the top-level document, whose origin
    # alone is its Referer under the default referrer policy.
    (navigation,) = frame_site.headers_of("/frame-page")
    assert (_value(navigation, "Sec-Fetch-Dest"), _value(navigation, "Sec-Fetch-Mode")) == ("iframe", "navigate")
    assert _value(navigation, "Sec-Fetch-Site") == "cross-site"
    assert _value(navigation, "Referer") == top.url("/")
    # Its fetch to its own origin is same-origin, from the frame's URL, but has no site for cookies under a top-level
    # document of another site, so its Lax cookie stays home, where a caller's fetch from that URL as a top-level
    # page takes it.
    (fetched,) = frame_site.headers_of("/frame-fetch")
    assert _value(fetched, "Sec-Fetch-Site") == "same-origin"
    assert _value(fetched, "Referer") == frame_site.url("/frame-page")
    assert _value(fetched, "Cookie") is None
    with mitto.Session() as session:
        session.get(frame_site.url("/set"))
        session.get(frame_site.url("/frame-fetch"), kind="fetch", initiator=frame_site.url("/frame-page"))
    assert _value(frame_site.headers_of("/frame-fetch")[-1], "Cookie") == "f=1"


def test_a_workers_requests_send_no_client_hints_and_its_script_as_referrer(
    tmp_path: Path, top: _Server, frame_site: _Server
) -> None:
    _run_page(tmp_path, top, frame_site)

    (script,) = top.headers_of("/worker.js")
    assert (_value(script, "Sec-Fetch-Dest"), _value(script, "Sec-Fetch-Mode")) == ("worker", "same-origin")
    assert _value(script, "Referer") == top.url("/page")
    (fetched,) = top.headers_of("/worker-fetch")
    assert _value(fetched, "Referer") == top.url("/worker.js")
    assert _value(fetched, "Sec-Fetch-Site") == "same-origin"
    assert [name for name, _ in fetched if name.lower().startswith("sec-ch-")] == []
    # The document's own fetch sends the hints that go by default.
    with mitto.Session() as session:
        session.get(top.url("/worker-fetch"), kind="fetch", initiator=top.url("/page"))
    assert _value(top.headers_of("/worker-fetch")[-1], "sec-ch-ua") is not None
