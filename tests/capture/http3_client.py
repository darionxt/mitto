"""An HTTP/3 client for tests, on aioquic, that stands in for what a real client does over real UDP sockets."""

import socket
import ssl
import time

from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import ConnectionTerminated

from tools.capture import DEFAULT_HOST

_TIMEOUT_SECONDS = 10.0


class Http3Client:
    """Just enough of an HTTP/3 client to send GETs and read the answers, over a real UDP socket."""

    def __init__(self, port: int, *, address: str = "127.0.0.1", alpn: list[str] | None = None) -> None:
        configuration = QuicConfiguration(
            is_client=True,
            alpn_protocols=H3_ALPN if alpn is None else alpn,
            server_name=DEFAULT_HOST,
            verify_mode=ssl.CERT_NONE,
        )
        self.quic = QuicConnection(configuration=configuration)
        on_ipv6 = ":" in address
        self.sock = socket.socket(socket.AF_INET6 if on_ipv6 else socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.05)
        # aioquic drops datagrams from any address but this one, and an IPv6 socket reports four fields.
        self.address = (address, port, 0, 0) if on_ipv6 else (address, port)
        self.quic.connect(self.address, time.monotonic())
        self.http = H3Connection(self.quic)
        self.statuses: dict[int, bytes] = {}
        self.bodies: dict[int, bytearray] = {}
        self.finished: set[int] = set()
        self.terminated: ConnectionTerminated | None = None
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
            for http_event in self.http.handle_event(event):
                if isinstance(http_event, HeadersReceived):
                    self.statuses[http_event.stream_id] = dict(http_event.headers)[b":status"]
                    if http_event.stream_ended:
                        self.finished.add(http_event.stream_id)
                elif isinstance(http_event, DataReceived):
                    self.bodies.setdefault(http_event.stream_id, bytearray()).extend(http_event.data)
                    if http_event.stream_ended:
                        self.finished.add(http_event.stream_id)
        self.flush()

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
