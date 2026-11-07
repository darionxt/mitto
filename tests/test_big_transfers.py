"""Moving 1 GiB each way through the public API, which has to hold no more of it than it holds of a small body.

The bytes are made as they go and thrown away as they come, so this takes neither disk nor much memory on either side.
Each test takes a few seconds. tests/big_transfer_client.py is the client, in a process of its own.
"""

import http.server
import json
import os
import socketserver
import ssl
import subprocess
import sys
import threading
import zlib
from collections.abc import Generator
from contextlib import contextmanager, suppress
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, urlsplit

import pytest
from cryptography.hazmat.primitives import serialization
from hpack import Decoder

from mitto._native import find_library
from tests.local_certificates import generate_leaf, generate_root
from tests.local_server import LocalHTTPServer
from tools.capture.http2 import (
    DATA,
    END_HEADERS,
    END_STREAM,
    HEADERS,
    PREFACE,
    SETTINGS,
    WINDOW_UPDATE,
    Frame,
    FrameParser,
    serialize_frame,
)

_GIB = 1024**3
_RUN_SECONDS = 600
_READ_SIZE = 1024 * 1024
_PART = bytes(range(256)) * 256

# What the process may grow by while it moves 1 GiB each way. Holding a body would take at least 1 GiB.
_MOST_GROWTH_BYTES = 64 * 1024 * 1024

_CLIENT = Path(__file__).with_name("big_transfer_client.py")

_HTTP2_ACK = 0x1
_MAX_HEADER_LIST_SIZE = 64 * 1024
_SETTINGS_INITIAL_WINDOW_SIZE = 4
_HTTP2_MAX_WINDOW = 2**31 - 1
_HTTP2_FRAME_SIZE = 16 * 1024
_HTTP2_WINDOW_STEP = 16 * 1024 * 1024
_HTTP2_DEFAULT_WINDOW = 65535
# HPACK's static table holds these, and the literal below needs no table.
_HPACK_STATUS_200 = b"\x88"


def _checksum_of_parts(size: int) -> int:
    checksum = 0
    left = size
    while left > 0:
        part = _PART[: min(left, len(_PART))]
        checksum = zlib.crc32(part, checksum)
        left -= len(part)
    return checksum


@dataclass(slots=True)
class _Tally:
    """What a server has received of the last upload."""

    received: int = 0
    checksum: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)

    def start(self) -> None:
        with self.lock:
            self.received = 0
            self.checksum = 0

    def add(self, data: bytes | memoryview) -> None:
        with self.lock:
            self.received += len(data)
            self.checksum = zlib.crc32(data, self.checksum)


class _SinkAndSource(http.server.BaseHTTPRequestHandler):
    """Reads an upload and throws it away, and writes a download as it goes."""

    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:
        tally = cast(Any, self.server).tally
        tally.start()
        left = int(self.headers["Content-Length"])
        while left > 0:
            data = self.rfile.read(min(left, _READ_SIZE))
            assert data
            tally.add(data)
            left -= len(data)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        size = int(parse_qs(urlsplit(self.path).query)["size"][0])
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()
        left = size
        while left > 0:
            part = _PART[: min(left, len(_PART))]
            self.wfile.write(part)
            left -= len(part)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@contextmanager
def _serving(server: LocalHTTPServer) -> Generator[None]:
    """Run `server` on a thread of its own until the with statement ends.

    shutdown() is what ends it. On Linux, closing a listening socket does not wake a thread that waits in accept() on it,
    so a loop of accept() would wait there for a connection that never comes.
    """
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


@contextmanager
def _http1_server() -> Generator[tuple[str, _Tally]]:
    server: Any = LocalHTTPServer(("127.0.0.1", 0), _SinkAndSource)
    server.tally = _Tally()
    with _serving(server):
        yield f"http://127.0.0.1:{server.server_port}/", server.tally


class _Http2Connection:
    """The least of an HTTP/2 server that takes an upload and gives a download, keeping to the client's flow control."""

    def __init__(self, connection: ssl.SSLSocket, tally: _Tally) -> None:
        self._connection = connection
        self._tally = tally
        self._parser = FrameParser()
        self._unread = bytearray()
        self._send_window = _HTTP2_DEFAULT_WINDOW
        self._stream_send_windows: dict[int, int] = {}
        self._stream_initial_window = _HTTP2_DEFAULT_WINDOW
        self._to_acknowledge = 0
        self._paths: dict[int, str] = {}
        self._decoder = Decoder(max_header_list_size=_MAX_HEADER_LIST_SIZE)

    def serve(self) -> None:
        self._read_preface()
        self._connection.sendall(
            serialize_frame(SETTINGS, 0, 0, _SETTINGS_INITIAL_WINDOW_SIZE.to_bytes(2) + _HTTP2_MAX_WINDOW.to_bytes(4))
            + serialize_frame(WINDOW_UPDATE, 0, 0, _HTTP2_WINDOW_STEP.to_bytes(4))
        )
        while frames := self._read_frames():
            for frame in frames:
                self._handle(frame)

    def _read_preface(self) -> None:
        preface = b""
        while len(preface) < len(PREFACE):
            data = self._connection.recv(len(PREFACE) - len(preface))
            if not data:
                raise ConnectionError("no preface")
            preface += data

    def _read_frames(self) -> list[Frame]:
        while True:
            try:
                data = self._connection.recv(_READ_SIZE)
            except OSError:
                return []
            if not data:
                return []
            frames = self._parser.feed(data)
            if frames:
                return frames

    def _handle(self, frame: Frame) -> None:
        if frame.type == SETTINGS and not frame.flags & _HTTP2_ACK:
            self._take_settings(frame.payload)
            self._connection.sendall(serialize_frame(SETTINGS, _HTTP2_ACK, 0))
        elif frame.type == WINDOW_UPDATE:
            self._take_window(frame)
        elif frame.type == HEADERS:
            self._take_headers(frame)
        elif frame.type == DATA:
            self._take_data(frame)

    def _take_settings(self, payload: bytes) -> None:
        for offset in range(0, len(payload), 6):
            if int.from_bytes(payload[offset : offset + 2]) == _SETTINGS_INITIAL_WINDOW_SIZE:
                self._stream_initial_window = int.from_bytes(payload[offset + 2 : offset + 6])

    def _take_window(self, frame: Frame) -> None:
        increment = int.from_bytes(frame.payload) & _HTTP2_MAX_WINDOW
        if frame.stream_id == 0:
            self._send_window += increment
        else:
            self._stream_send_windows[frame.stream_id] = (
                self._stream_send_windows.get(frame.stream_id, self._stream_initial_window) + increment
            )

    def _take_headers(self, frame: Frame) -> None:
        # Past the priority fields, which //net always sends.
        headers: dict[str, str] = dict(cast(Any, self._decoder.decode(frame.payload[5:])))
        self._paths[frame.stream_id] = headers[":path"]
        if headers[":method"] == "POST":
            self._tally.start()
        if frame.flags & END_STREAM:
            self._answer(frame.stream_id)

    def _take_data(self, frame: Frame) -> None:
        self._tally.add(frame.payload)
        self._to_acknowledge += len(frame.payload)
        if self._to_acknowledge >= _HTTP2_WINDOW_STEP // 2:
            self._connection.sendall(serialize_frame(WINDOW_UPDATE, 0, 0, self._to_acknowledge.to_bytes(4)))
            self._to_acknowledge = 0
        if frame.flags & END_STREAM:
            self._answer(frame.stream_id)

    def _answer(self, stream_id: int) -> None:
        query = parse_qs(urlsplit(self._paths[stream_id]).query)
        size = int(query["size"][0]) if "size" in query else 0
        self._connection.sendall(
            serialize_frame(HEADERS, END_HEADERS | (0 if size else END_STREAM), stream_id, _HPACK_STATUS_200)
        )
        if size:
            self._send_body(stream_id, size)

    def _send_body(self, stream_id: int, size: int) -> None:
        left = size
        offset = 0
        while left > 0:
            allowed = self._window_for(stream_id)
            while allowed == 0:
                frames = self._read_frames()
                if not frames:
                    return
                for frame in frames:
                    self._handle(frame)
                allowed = self._window_for(stream_id)
            length = min(left, allowed, _HTTP2_FRAME_SIZE, len(_PART) - offset)
            flags = END_STREAM if left == length else 0
            self._connection.sendall(serialize_frame(DATA, flags, stream_id, _PART[offset : offset + length]))
            offset = (offset + length) % len(_PART)
            left -= length
            self._send_window -= length
            self._stream_send_windows[stream_id] = self._window_of_stream(stream_id) - length

    def _window_of_stream(self, stream_id: int) -> int:
        return self._stream_send_windows.get(stream_id, self._stream_initial_window)

    def _window_for(self, stream_id: int) -> int:
        return max(min(self._send_window, self._window_of_stream(stream_id)), 0)


class _Http2Handler(socketserver.BaseRequestHandler):
    """Serves a connection over TLS with _Http2Connection."""

    def handle(self) -> None:
        server = cast(Any, self.server)
        with (
            server.context.wrap_socket(self.request, server_side=True) as connection,
            suppress(ConnectionError, ssl.SSLError),
        ):
            _Http2Connection(connection, server.tally).serve()


@contextmanager
def _http2_server(directory: Path) -> Generator[tuple[str, _Tally, str]]:
    root = generate_root()
    leaf = generate_leaf(root, "localhost")
    key = leaf.key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    ).decode()
    chain_file = directory / "chain.pem"
    chain_file.write_text(leaf.pem + key)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(chain_file)
    context.set_alpn_protocols(["h2"])
    server: Any = LocalHTTPServer(("127.0.0.1", 0), _Http2Handler)
    server.context = context
    server.tally = _Tally()
    with _serving(server):
        yield f"https://localhost:{server.server_port}/", server.tally, root.pem


def _run_client(api: str, url: str, *arguments: str) -> dict[str, Any]:
    if find_library() is None:
        pytest.skip("no native library to run this with")
    through_asyncio = ["--asyncio"] if api == "asyncio" else []
    result = subprocess.run(
        [sys.executable, _CLIENT, url, str(_GIB), *through_asyncio, *arguments],
        capture_output=True,
        text=True,
        timeout=_RUN_SECONDS,
        env=os.environ,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_1_gib_goes_each_way_over_http1_with_flat_memory(both_apis: str) -> None:
    with _http1_server() as (url, tally):
        outcome = _run_client(both_apis, url, "--known-size")

    assert outcome["protocol"] == "http/1.1"
    assert outcome["status"] == 200
    assert tally.received == _GIB
    assert outcome["received"] == _GIB
    checksum = _checksum_of_parts(_GIB)
    assert tally.checksum == checksum
    assert outcome["checksum"] == checksum
    assert outcome["growth"] < _MOST_GROWTH_BYTES, outcome


def test_1_gib_goes_each_way_over_http2_with_no_known_size_and_flat_memory(both_apis: str, tmp_path: Path) -> None:
    with _http2_server(tmp_path) as (url, tally, anchor):
        outcome = _run_client(both_apis, url, "--trust-anchor", anchor)

    assert outcome["protocol"] == "h2"
    assert outcome["status"] == 200
    assert tally.received == _GIB
    assert outcome["received"] == _GIB
    checksum = _checksum_of_parts(_GIB)
    assert tally.checksum == checksum
    assert outcome["checksum"] == checksum
    assert outcome["growth"] < _MOST_GROWTH_BYTES, outcome
