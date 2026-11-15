"""Tests of the storage keys that the library works out for the localStorage of a Page's frames, from the frame tree.

The Page program makes no frame yet, so a fake Page program stands in for it, as in tests/test_page_contexts.py: a
Python script beside a copy of the library, which speaks the messages of native/page_messages.h. Under a top-level
document of 127.0.0.1 it commits a frame of localhost, another site, with a frame of 127.0.0.1 in it, and a frame's
initial about:blank of 127.0.0.1, and each binds localStorage, reads it and writes it. A Session in another process,
with a storage directory, opens the Page, and the test reads what the directory holds afterwards. Another fake Page
sends a change that Blink would not write, which the library refuses.
"""

import http.server
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import textwrap
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from mitto import _native
from tests.local_server import QuietServer

pytestmark = [
    pytest.mark.usefixtures("native_library"),
    pytest.mark.skipif(sys.platform == "win32", reason="the fake Page program is a script"),
]

_TIMEOUT_SECONDS = 60

# The version of the messages of native/page_messages.h.
_PROTOCOL_VERSION = 22

_FAKE_PAGE = textwrap.dedent(
    """\
    #!{python}
    # Stands in for the Page program: a top-level document, frames in it, and the localStorage of each.
    import json
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

    def frame(parent, blank, navigation, document, url):
        # A frame's initial about:blank, then its navigation, of Kind iframe, whose document takes its place.
        send(70, struct.pack("<QBQ", blank, 0, parent))
        send(8, request(parent, navigation, 11, url, target=blank))
        wait_for_end(navigation)
        send(70, struct.pack("<QBQQ", document, 1, parent, navigation))

    # localStorage's keys and values, after Blink's byte for Latin-1.
    def bind(document, area, observer):
        send(27, struct.pack("<QQBQ", document, area, 1, observer))

    def put(area, key, value):
        source = struct.pack("<QQ", 1, area) + sized(b"http://source.test/")
        send(29, struct.pack("<Q", area) + source + sized(b"\\x01" + key) + b"\\x01" + value)

    def entries(area, observer):
        send(28, struct.pack("<QQ", area, observer))
        found = {{}}
        while True:
            message_type, payload = receive()
            if message_type != 33 or struct.unpack_from("<Q", payload)[0] != area:
                continue
            rest = payload[9:]
            while rest:
                key_size = struct.unpack_from("<I", rest)[0]
                key = rest[4 : 4 + key_size]
                rest = rest[4 + key_size :]
                value_size = struct.unpack_from("<I", rest)[0]
                value = rest[4 : 4 + value_size]
                rest = rest[4 + value_size :]
                found[key[1:].decode("latin-1")] = value[1:].decode("latin-1")
            if payload[8] == 1:
                return found

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
    {actions}
    while channel.recv(65536):
        pass
    """
)

# What the fake Page does once the Session asks it something: it commits a frame of the other site under the top-level
# document, with a frame of the top-level site in it, and a frame's initial about:blank, and each binds localStorage,
# reads it and writes it.
_FRAME_ACTIONS = textwrap.dedent(
    """\
    frame(2, 3, 1, 4, "{frame}")
    frame(4, 5, 2, 6, "{nested}")
    send(70, struct.pack("<QBQ", 7, 0, 2))

    bind(2, 1, 1)
    put(1, b"k", b"top")
    seen = {{}}
    bind(4, 2, 2)
    seen["frame"] = entries(2, 3)
    put(2, b"k", b"frame")
    bind(6, 3, 4)
    seen["nested"] = entries(3, 5)
    put(3, b"k", b"nested")
    bind(7, 4, 6)
    seen["blank"] = entries(4, 7)
    send(5, struct.pack("<QB", call, 0) + json.dumps(seen).encode())
    """
)

# A key and value of localStorage with no byte for their format, which Blink always writes.
_EMPTY_PUT_ACTIONS = textwrap.dedent(
    """\
    bind(2, 1, 1)
    source = struct.pack("<QQ", 1, 1) + sized(b"http://source.test/")
    send(29, struct.pack("<Q", 1) + source + sized(b""))
    """
)

# Opens a Page at the URL in argv[1] in a Session that keeps its storage in argv[2], and prints what it gives, or the
# error that the call raised.
_SESSION = textwrap.dedent(
    """\
    import json
    import sys

    import mitto

    with mitto.Session(_storage=sys.argv[2]) as session:
        with session._open_page(sys.argv[1], timeout=60) as page:
            try:
                print(json.dumps(page.evaluate("storage", timeout=60)))
            except Exception as error:
                print(json.dumps({"error": str(error)}))
    """
)


class _Server:
    """A local server that answers each path with a small page."""

    def __init__(self, host: str) -> None:
        self.host = host

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                body = b"<p>page</p>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self._server.server_port

    def url(self, path: str) -> str:
        return f"http://{self.host}:{self.port}{path}"

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


def _run_page(tmp_path: Path, top: _Server, actions: str, storage: Path) -> Any:
    """Run the fake Page with `actions` in a Session of its own that keeps its storage in `storage`, and return what
    it gave."""
    library = _native.find_library()
    assert library is not None
    directory = tmp_path / "library"
    directory.mkdir()
    copy = directory / library.name
    shutil.copy2(library, copy)
    page_program = directory / _native.PAGE_PROGRAM_NAME
    page_program.write_text(_FAKE_PAGE.format(python=sys.executable, version=_PROTOCOL_VERSION, actions=actions))
    page_program.chmod(0o755)
    environment = {**os.environ, _native.LIBRARY_ENV_VAR: str(copy)}
    finished = subprocess.run(
        [sys.executable, "-c", _SESSION, top.url("/page"), str(storage)],
        env=environment,
        check=True,
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS,
    )
    return json.loads(finished.stdout)


def test_a_frames_local_storage_is_partitioned_by_the_top_level_site(
    tmp_path: Path, top: _Server, frame_site: _Server
) -> None:
    storage = tmp_path / "storage"
    actions = _FRAME_ACTIONS.format(frame=frame_site.url("/frame-page"), nested=top.url("/nested-page"))
    seen = _run_page(tmp_path, top, actions, storage)

    # A frame of 127.0.0.1 under one of another site has storage apart from the top-level document's, of the same
    # origin, and a frame's about:blank of the top-level document's origin, directly under it, shares the top-level
    # document's.
    assert seen == {"frame": {}, "nested": {}, "blank": {"k": "top"}}
    # Each key as Blink's StorageKey::Serialize() writes it: the top-level site after "^0" for a frame of another
    # site, and the ancestor chain bit after "^3" for a frame of the top-level site under one of another site.
    with sqlite3.connect(storage / "LocalStorage") as database:
        held = dict(
            database.execute(
                "SELECT maps.storage_key, map_entries.value FROM maps JOIN map_entries ON map_entries.map_id = maps.row_id"
            ).fetchall()
        )
    assert held == {
        f"http://127.0.0.1:{top.port}/".encode(): b"\x01top",
        f"http://localhost:{frame_site.port}/^0http://127.0.0.1".encode(): b"\x01frame",
        f"http://127.0.0.1:{top.port}/^31".encode(): b"\x01nested",
    }


def test_a_change_of_a_key_that_blink_does_not_write_is_refused(tmp_path: Path, top: _Server) -> None:
    # A key of localStorage that has no byte for its format, which the Session's store counts on.
    seen = _run_page(tmp_path, top, _EMPTY_PUT_ACTIONS, tmp_path / "storage")

    assert seen["error"].endswith(
        "the library refused a message from the Page: a change to storage of a key or value that Blink does not write"
    )
