"""Clients for tests, over real sockets and real TLS, with Python's own TLS as what speaks it."""

import re
import socket
import ssl
from collections.abc import Sequence
from dataclasses import dataclass

from hpack import Decoder, Encoder

from tests.capture import samples
from tools.capture import DEFAULT_HOST
from tools.capture.http2 import DATA, END_HEADERS, END_STREAM, HEADERS, PREFACE, SETTINGS, FrameParser, serialize_frame

_TIMEOUT_SECONDS = 10.0


@dataclass(frozen=True, slots=True)
class Answer:
    """A response, with the names of its headers in lower case."""

    status: int
    headers: dict[str, str]
    body: bytes


def client_context(offered: tuple[str, ...] = ("h2", "http/1.1")) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.set_alpn_protocols(list(offered))
    return context


def connect(port: int, offered: tuple[str, ...] = ("h2", "http/1.1"), host: str = DEFAULT_HOST) -> ssl.SSLSocket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=_TIMEOUT_SECONDS)
    return client_context(offered).wrap_socket(sock, server_hostname=host)


def read_http1_answer(sock: socket.socket) -> bytes:
    """Return a whole response, which has to say how long it is."""
    received = bytearray()
    while b"\r\n\r\n" not in received:
        received += sock.recv(1 << 16)
    head, _, body = bytes(received).partition(b"\r\n\r\n")
    match = re.search(rb"Content-Length: (\d+)", head)
    assert match is not None
    missing = int(match.group(1)) - len(body)
    while missing > 0:
        more = sock.recv(1 << 16)
        body += more
        missing -= len(more)
    return head + b"\r\n\r\n" + body


def http1_get(sock: ssl.SSLSocket, target: str) -> bytes:
    """Send a GET and return the whole response."""
    sock.sendall(f"GET {target} HTTP/1.1\r\nHost: {sock.server_hostname}\r\nAccept: */*\r\n\r\n".encode())
    return read_http1_answer(sock)


def http1_exchange(
    sock: socket.socket,
    method: str,
    target: str,
    *,
    authority: str,
    headers: Sequence[tuple[str, str]] = (),
    body: bytes = b"",
) -> Answer:
    """Send a request and return its answer."""
    lines = [f"{method} {target} HTTP/1.1", f"Host: {authority}", *(f"{name}: {value}" for name, value in headers)]
    if body:
        lines.append(f"Content-Length: {len(body)}")
    sock.sendall("\r\n".join(lines).encode("latin-1") + b"\r\n\r\n" + body)
    head, _, content = read_http1_answer(sock).partition(b"\r\n\r\n")
    status_line, *header_lines = head.decode("latin-1").split("\r\n")
    fields = (line.partition(": ") for line in header_lines)
    return Answer(int(status_line.split(" ")[1]), {name.lower(): value for name, _, value in fields}, content)


class Http2Client:
    """Just enough of an HTTP/2 client to send requests and read the answers."""

    def __init__(self, sock: ssl.SSLSocket) -> None:
        self.sock = sock
        self.parser = FrameParser()
        self.encoder = Encoder()
        self.decoder = Decoder()
        self.next_stream = 1
        sock.sendall(PREFACE + serialize_frame(SETTINGS, 0, 0, (4).to_bytes(2) + (65536).to_bytes(4)))

    def get(self, target: str) -> tuple[bytes, bytes]:
        """Send a GET and return the status and the body."""
        stream_id = self.next_stream
        self.next_stream += 2
        self.sock.sendall(samples.http2_request_frame(target, stream_id=stream_id, encoder=self.encoder))
        answer = self._read_answer(stream_id)
        return str(answer.status).encode(), answer.body

    def exchange(
        self,
        method: str,
        target: str,
        *,
        authority: str,
        headers: Sequence[tuple[str, str]] = (),
        body: bytes = b"",
    ) -> Answer:
        """Send a request, with a body in one frame after its headers, and return its answer."""
        stream_id = self.next_stream
        self.next_stream += 2
        block = self.encoder.encode(
            [
                (":method", method),
                (":authority", authority),
                (":scheme", "https"),
                (":path", target),
                *((name.lower(), value) for name, value in headers),
            ]
        )
        if body:
            frames = serialize_frame(HEADERS, END_HEADERS, stream_id, block)
            frames += serialize_frame(DATA, END_STREAM, stream_id, body)
        else:
            frames = serialize_frame(HEADERS, END_STREAM | END_HEADERS, stream_id, block)
        self.sock.sendall(frames)
        return self._read_answer(stream_id)

    def _read_answer(self, stream_id: int) -> Answer:
        fields: dict[bytes, bytes] = {}
        body = bytearray()
        while True:
            for frame in self.parser.feed(self.sock.recv(1 << 16)):
                if frame.stream_id != stream_id:
                    continue
                if frame.type == HEADERS:
                    fields = dict(self.decoder.decode(frame.payload, raw=True))
                elif frame.type == DATA:
                    body += frame.payload
                if frame.type in {HEADERS, DATA} and frame.flags & END_STREAM:
                    status = int(fields[b":status"])
                    decoded = {name.decode(): value.decode() for name, value in fields.items()}
                    return Answer(status, decoded, bytes(body))
