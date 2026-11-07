"""An HTTP/3 client for tests, on aioquic, that stands in for what a real client does over real UDP sockets."""

import socket
import ssl
import time
from collections.abc import Sequence

from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted, StreamDataReceived
from aioquic.tls import SessionTicket

from tests.capture.clients import Answer
from tools.capture import DEFAULT_HOST
from tools.capture.http3 import GOAWAY, FrameParser

_TIMEOUT_SECONDS = 10.0
_SERVER_CONTROL_STREAM = 3


class _ServerControlStream:
    """Reads what the server says on its control stream, for a GOAWAY frame."""

    def __init__(self) -> None:
        self._parser = FrameParser()
        self._type_read = False

    def feed(self, data: bytes) -> int | None:
        """Return the stream number of a GOAWAY frame in data, if there is one."""
        if not self._type_read:
            # The stream's type is one byte here.
            self._type_read = True
            data = data[1:]
        goaway = None
        for frame_type, payload in self._parser.feed(data):
            if frame_type == GOAWAY:
                goaway = _read_varint(payload)
        return goaway


def _read_varint(data: bytes) -> int:
    length = 1 << (data[0] >> 6)
    return int.from_bytes(bytes([data[0] & 0x3F]) + data[1:length])


def session_ticket_after(client: "Http3Client") -> SessionTicket:
    """Return the ticket a client that keeps them got from the server, waiting for it if it has to."""
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while not client.tickets:
        assert time.monotonic() < deadline, "no ticket"
        client.pump()
    return client.tickets[-1]


class Http3Client:
    """Just enough of an HTTP/3 client to send GETs and read the answers, over a real UDP socket."""

    def __init__(
        self,
        port: int,
        *,
        address: str = "127.0.0.1",
        alpn: list[str] | None = None,
        host: str = DEFAULT_HOST,
        session_ticket: SessionTicket | None = None,
        keep_tickets: bool = False,
    ) -> None:
        configuration = QuicConfiguration(
            is_client=True,
            alpn_protocols=H3_ALPN if alpn is None else alpn,
            server_name=host,
            verify_mode=ssl.CERT_NONE,
            session_ticket=session_ticket,
        )
        self.tickets: list[SessionTicket] = []
        self.quic = QuicConnection(
            configuration=configuration, session_ticket_handler=self.tickets.append if keep_tickets else None
        )
        on_ipv6 = ":" in address
        self.sock = socket.socket(socket.AF_INET6 if on_ipv6 else socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.05)
        # aioquic drops datagrams from any address but this one, and an IPv6 socket reports four fields.
        self.address = (address, port, 0, 0) if on_ipv6 else (address, port)
        self.quic.connect(self.address, time.monotonic())
        self.http = H3Connection(self.quic)
        self.statuses: dict[int, bytes] = {}
        self.headers: dict[int, dict[str, str]] = {}
        self.bodies: dict[int, bytearray] = {}
        self.finished: set[int] = set()
        self.terminated: ConnectionTerminated | None = None
        self.handshake_done = False
        self.goaway: int | None = None
        self._control_stream = _ServerControlStream()
        self.flush()

    def __enter__(self) -> "Http3Client":
        return self

    def __exit__(self, *_: object) -> None:
        self.quic.close()
        self.flush()
        self.sock.close()

    def flush(self) -> None:
        for data, address in self.quic.datagrams_to_send(time.monotonic()):
            self.sock.sendto(data, address)

    def pump(self) -> None:
        """Send and receive for one round."""
        try:
            data, address = self.sock.recvfrom(1 << 16)
        except TimeoutError:
            pass
        else:
            self.quic.receive_datagram(data, address, time.monotonic())
        timer = self.quic.get_timer()
        if timer is not None and timer <= time.monotonic():
            self.quic.handle_timer(time.monotonic())
        while (event := self.quic.next_event()) is not None:
            if isinstance(event, ConnectionTerminated):
                self.terminated = event
            elif isinstance(event, HandshakeCompleted):
                self.handshake_done = True
            elif isinstance(event, StreamDataReceived) and event.stream_id == _SERVER_CONTROL_STREAM:
                self.goaway = self._control_stream.feed(event.data) or self.goaway
            for http_event in self.http.handle_event(event):
                if isinstance(http_event, HeadersReceived):
                    self.statuses[http_event.stream_id] = dict(http_event.headers)[b":status"]
                    self.headers[http_event.stream_id] = {
                        name.decode(): value.decode() for name, value in http_event.headers
                    }
                    if http_event.stream_ended:
                        self.finished.add(http_event.stream_id)
                elif isinstance(http_event, DataReceived):
                    self.bodies.setdefault(http_event.stream_id, bytearray()).extend(http_event.data)
                    if http_event.stream_ended:
                        self.finished.add(http_event.stream_id)
        self.flush()

    def wait_for_handshake(self) -> None:
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while not self.handshake_done:
            assert time.monotonic() < deadline, "no handshake"
            self.pump()

    def wait_for_goaway(self) -> int:
        """Return the stream number in the GOAWAY frame the server sends."""
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while self.goaway is None:
            assert time.monotonic() < deadline, "no GOAWAY"
            self.pump()
        return self.goaway

    def get(self, target: str) -> tuple[bytes, bytes]:
        """Send a GET and return the status and the body."""
        stream_id = self.quic.get_next_available_stream_id()
        self.http.send_headers(
            stream_id,
            [
                (b":method", b"GET"),
                (b":scheme", b"https"),
                (b":authority", f"{DEFAULT_HOST}:{self.address[1]}".encode()),
                (b":path", target.encode()),
                (b"x-mitto", b"test"),
            ],
            end_stream=True,
        )
        self.flush()
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while stream_id not in self.finished:
            assert time.monotonic() < deadline, "no answer"
            self.pump()
        return self.statuses[stream_id], bytes(self.bodies.get(stream_id, b""))

    def exchange(
        self,
        method: str,
        target: str,
        *,
        authority: str,
        headers: Sequence[tuple[str, str]] = (),
        body: bytes = b"",
    ) -> Answer:
        """Send a request and return its answer."""
        stream_id = self.quic.get_next_available_stream_id()
        self.http.send_headers(
            stream_id,
            [
                (b":method", method.encode()),
                (b":scheme", b"https"),
                (b":authority", authority.encode()),
                (b":path", target.encode()),
                *((name.lower().encode(), value.encode()) for name, value in headers),
            ],
            end_stream=not body,
        )
        if body:
            self.http.send_data(stream_id, body, end_stream=True)
        self.flush()
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while stream_id not in self.finished:
            assert time.monotonic() < deadline, "no answer"
            self.pump()
        return Answer(int(self.statuses[stream_id]), self.headers[stream_id], bytes(self.bodies.get(stream_id, b"")))
