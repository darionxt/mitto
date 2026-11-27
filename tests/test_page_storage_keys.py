"""Tests of the storage keys that the library works out for the localStorage and IndexedDB of a Page's frames, from the
frame tree.

The Page program makes no frame yet, so a fake Page program stands in for it, as in tests/test_page_contexts.py: a
Python script beside a copy of the library, which speaks the messages of native/page_messages.h. Under a top-level
document of 127.0.0.1 it commits a frame of localhost, another site, with a frame of 127.0.0.1 in it, and a frame's
initial about:blank of 127.0.0.1, and each binds localStorage, reads it and writes it, lists and opens databases, or
opens and lists buckets. A Session in another process, with a storage directory, opens the Page, and the test reads what
the directory holds afterwards. Other fake Pages send what Blink would not, which the library refuses.
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
from contextlib import closing
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

    def hello():
        # The library's hello comes first: speak its version back, as a Page of the same tree does.
        message_type, payload = receive()
        assert message_type == 1, message_type
        send(1, payload)

    def sized(data):
        return struct.pack("<I", len(data)) + data

    def request(document, number, kind, url, target=0):
        return (
            struct.pack("<QQBB", document, number, kind, 0)
            + struct.pack("<BBBBBBBQ", 0, 0, 0, 0, 0, 0, 0, target)
            + sized(url.encode()) + sized(b"GET") + sized(b"") + sized(b"") + b"\\x00"
        )

    def wait_for_end(number):
        while True:
            message_type, payload = receive()
            if message_type == 15 and struct.unpack_from("<Q", payload)[0] == number:
                return

    def frame(parent, blank, navigation, document, url):
        # A frame's initial about:blank, then its navigation, of Kind iframe, whose document takes its place.
        send(70, struct.pack("<QBBQ", blank, 0, 0, parent))
        send(8, request(parent, navigation, 11, url, target=blank))
        wait_for_end(navigation)
        send(70, struct.pack("<QBBQQ", document, 1, 0, parent, navigation))

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

    # IndexedDB's calls, after the document's ID, and the answers to those of them that have a number. A factory's call
    # names the default bucket, 0, after its number.
    def idb(document, payload):
        send(42, struct.pack("<Q", document) + payload)

    def idb_answer(call):
        while True:
            message_type, payload = receive()
            if message_type == 43 and payload[0] in (1, 3, 4, 6) and struct.unpack_from("<Q", payload, 1)[0] == call:
                return payload

    def idb_open(document, call, connection, transaction):
        # Database d at version 1, whose upgrade commits at once.
        name = "d".encode("utf-16-le")
        idb(document, struct.pack("<BQQQQI", 2, call, 0, connection, transaction, 1) + name + struct.pack("<q", 1))
        assert idb_answer(call)[0] == 3
        idb(document, struct.pack("<BQq", 25, transaction, 0))
        assert idb_answer(call)[0] == 4

    def idb_names(document, call):
        idb(document, struct.pack("<BQQ", 1, call, 0))
        # After the operation, the call, the error and its message: how many databases, and each name and version.
        rest = idb_answer(call)[10:]
        rest = rest[4 + 2 * struct.unpack_from("<I", rest)[0] :]
        count = struct.unpack_from("<I", rest)[0]
        rest = rest[4:]
        names = []
        for _ in range(count):
            size = struct.unpack_from("<I", rest)[0]
            names.append(rest[4 : 4 + 2 * size].decode("utf-16-le"))
            rest = rest[4 + 2 * size + 8 :]
        return names

    # The Storage Buckets API's calls, after the document's ID, and their answers.
    def buckets(document, payload):
        send(180, struct.pack("<Q", document) + payload)

    def bucket_answer(question):
        while True:
            message_type, payload = receive()
            if message_type == 181 and struct.unpack_from("<Q", payload, 1)[0] == question:
                return payload

    def bucket_open(document, question, name):
        # No policies; the answer's error, then the bucket's number.
        buckets(document, struct.pack("<BQ", 1, question) + sized(name) + b"\\x00")
        answer = bucket_answer(question)
        assert answer[9] == 0
        return struct.unpack_from("<Q", answer, 10)[0]

    def bucket_keys(document, question):
        buckets(document, struct.pack("<BQ", 2, question))
        rest = bucket_answer(question)[10:]
        count = struct.unpack_from("<I", rest)[0]
        rest = rest[4:]
        names = []
        for _ in range(count):
            size = struct.unpack_from("<I", rest)[0]
            names.append(rest[4 : 4 + size].decode())
            rest = rest[4 + size :]
        return names

    hello()
    send(70, struct.pack("<QBBQ", 1, 0, 0, 0))
    wait_for_end(0)
    send(70, struct.pack("<QBBQQ", 2, 1, 0, 0, 0))
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
    send(70, struct.pack("<QBBQ", 7, 0, 0, 2))

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

# The same frames, but each lists the databases it sees once the top-level document has opened one, and the two frames
# of other storage keys open one of their own.
_INDEXED_DB_ACTIONS = textwrap.dedent(
    """\
    frame(2, 3, 1, 4, "{frame}")
    frame(4, 5, 2, 6, "{nested}")
    send(70, struct.pack("<QBBQ", 7, 0, 0, 2))

    idb_open(2, 1, 1, 1)
    seen = {{}}
    seen["frame"] = idb_names(4, 2)
    seen["nested"] = idb_names(6, 3)
    seen["blank"] = idb_names(7, 4)
    idb_open(4, 5, 2, 2)
    idb_open(6, 6, 3, 3)
    send(5, struct.pack("<QB", call, 0) + json.dumps(seen).encode())
    """
)

# A frame of the other site opens a bucket, and it and the top-level document list theirs.
_BUCKET_ACTIONS = textwrap.dedent(
    """\
    frame(2, 3, 1, 4, "{frame}")
    seen = {{}}
    seen["frame_bucket"] = bucket_open(4, 1, b"f")
    seen["frame_keys"] = bucket_keys(4, 2)
    seen["top_keys"] = bucket_keys(2, 3)
    send(5, struct.pack("<QB", call, 0) + json.dumps(seen).encode())
    """
)

# An open of a bucket whose name Blink would not send.
_BAD_BUCKET_NAME_ACTIONS = textwrap.dedent(
    """\
    buckets(2, struct.pack("<BQ", 1, 1) + sized(b"Inbox") + b"\\x00")
    """
)

# A list of databases of a bucket that the Session never gave the Page.
_UNGIVEN_BUCKET_ACTIONS = textwrap.dedent(
    """\
    idb(2, struct.pack("<BQQ", 1, 1, 7))
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
    page_program.write_text(_FAKE_PAGE.format(python=sys.executable, actions=actions))
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


def test_a_frames_databases_are_partitioned_by_the_top_level_site(
    tmp_path: Path, top: _Server, frame_site: _Server
) -> None:
    storage = tmp_path / "storage"
    actions = _INDEXED_DB_ACTIONS.format(frame=frame_site.url("/frame-page"), nested=top.url("/nested-page"))
    seen = _run_page(tmp_path, top, actions, storage)

    # Only the frame's about:blank of the top-level document's origin, directly under it, sees its database.
    assert seen == {"frame": [], "nested": [], "blank": ["d"]}
    # The file keys each database's entries as localStorage keys its maps.
    with closing(sqlite3.connect(storage / "IndexedDB")) as database:
        held = {key for (key,) in database.execute("SELECT DISTINCT storage_key FROM entries").fetchall()}
    assert held == {
        f"http://127.0.0.1:{top.port}/".encode(),
        f"http://localhost:{frame_site.port}/^0http://127.0.0.1".encode(),
        f"http://127.0.0.1:{top.port}/^31".encode(),
    }


def test_a_change_of_a_key_that_blink_does_not_write_is_refused(tmp_path: Path, top: _Server) -> None:
    # A key of localStorage that has no byte for its format, which the Session's store counts on.
    seen = _run_page(tmp_path, top, _EMPTY_PUT_ACTIONS, tmp_path / "storage")

    assert seen["error"].endswith(
        "the library refused a message from the Page: a change to storage of a key or value that Blink does not write"
    )


def test_a_frames_buckets_are_partitioned_by_the_top_level_site(
    tmp_path: Path, top: _Server, frame_site: _Server
) -> None:
    storage = tmp_path / "storage"
    seen = _run_page(tmp_path, top, _BUCKET_ACTIONS.format(frame=frame_site.url("/frame-page")), storage)

    assert seen == {"frame_bucket": 1, "frame_keys": ["f"], "top_keys": []}
    # The file keeps the bucket under the frame's storage key, as Chrome's QuotaManager database does.
    with closing(sqlite3.connect(storage / "QuotaManager")) as database:
        held = database.execute("SELECT storage_key, name FROM buckets").fetchall()
    assert held == [(f"http://localhost:{frame_site.port}/^0http://127.0.0.1", "f")]


def test_an_open_of_a_bucket_whose_name_blink_does_not_send_is_refused(tmp_path: Path, top: _Server) -> None:
    seen = _run_page(tmp_path, top, _BAD_BUCKET_NAME_ACTIONS, tmp_path / "storage")

    assert seen["error"].endswith(
        "the library refused a message from the Page: an open of a bucket whose name is not valid"
    )


def test_a_call_of_a_bucket_the_page_was_not_given_is_refused(tmp_path: Path, top: _Server) -> None:
    seen = _run_page(tmp_path, top, _UNGIVEN_BUCKET_ACTIONS, tmp_path / "storage")

    assert seen["error"].endswith(
        "the library refused a message from the Page: a call of the storage of bucket 7, which the Page was not given"
    )
