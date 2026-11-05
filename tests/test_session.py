"""Tests for what a native Session does on the wire: its settings, its connections and its request's headers.

All of them go through the real native core to a local server, so they need no network connection, and are skipped when
this machine has no native library.

data/private_certificate.pem holds a self-signed certificate and its key, for tests only. It was made with:

    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out cert.pem \
        -days 36500 -subj "/CN=mitto test" -addext "subjectAltName=IP:127.0.0.1" -sha256
    cat cert.pem key.pem > private_certificate.pem

and PRIVATE_CERTIFICATE_SPKI_HASH with:

    openssl x509 -in private_certificate.pem -pubkey -noout | openssl pkey -pubin -outform der | \
        openssl dgst -sha256 -binary | base64
"""

import concurrent.futures
import http.server
import socket
import socketserver
import ssl
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto._native import Priority, RequestError, Session

pytestmark = pytest.mark.usefixtures("native_library")

ERR_CERT_AUTHORITY_INVALID = -202

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="

# A valid hash of some other key: the Base64 form of 32 bytes.
OTHER_SPKI_HASH = "A" * 43 + "="


@dataclass(frozen=True, slots=True)
class ReceivedRequest:
    """What a server saw of one request."""

    client_address: tuple[str, int]
    headers: list[tuple[str, str]]


class RecordingServer(http.server.ThreadingHTTPServer):
    """An HTTP/1.1 server that keeps connections open and records each request."""

    # A Session opens up to six connections to a host at once, and the default queue holds five. macOS makes room in a
    # full queue by dropping a half-open connection, which the client sees as a reset.
    request_queue_size = socket.SOMAXCONN

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, RecordingHandler)
        self.received: list[ReceivedRequest] = []
        self.connection_closed = threading.Event()

    @property
    def base_url(self) -> str:
        return f"127.0.0.1:{self.server_port}/"


class RecordingHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast(RecordingServer, self.server)
        server.received.append(ReceivedRequest(self.client_address, self.headers.items()))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.send_header("Set-Cookie", "a=1")
        self.end_headers()

    def finish(self) -> None:
        super().finish()
        cast(RecordingServer, self.server).connection_closed.set()

    def log_message(self, format: str, *args: Any) -> None:
        pass


def serve(server: socketserver.BaseServer) -> None:
    # A short interval, because shutdown() takes this long to be noticed.
    threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()


def private_certificate_context() -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(PRIVATE_CERTIFICATE)
    return context


@pytest.fixture
def http_server() -> Iterator[RecordingServer]:
    """Run a local HTTP server."""
    with RecordingServer(("127.0.0.1", 0)) as server:
        serve(server)
        yield server
        server.shutdown()


@pytest.fixture
def tls_server() -> Iterator[RecordingServer]:
    """Run a local HTTPS server with a certificate that no root vouches for."""
    with RecordingServer(("127.0.0.1", 0)) as server:
        server.socket = private_certificate_context().wrap_socket(server.socket, server_side=True)
        serve(server)
        yield server
        server.shutdown()


def test_a_private_certificate_is_refused_by_default(tls_server: RecordingServer) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.request(f"https://{tls_server.base_url}")

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID
    assert tls_server.received == []


def test_a_private_certificate_is_accepted_when_its_spki_hash_is_listed(tls_server: RecordingServer) -> None:
    with Session(ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH]) as session:
        response = session.request(f"https://{tls_server.base_url}")

    assert response.status == 200
    assert len(tls_server.received) == 1


def test_a_private_certificate_is_refused_when_only_another_hash_is_listed(tls_server: RecordingServer) -> None:
    with (
        Session(ignore_certificate_errors_spki_list=[OTHER_SPKI_HASH]) as session,
        pytest.raises(RequestError) as caught,
    ):
        session.request(f"https://{tls_server.base_url}")

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID
    assert tls_server.received == []


def test_a_listed_spki_hash_applies_only_to_its_own_session(tls_server: RecordingServer) -> None:
    url = f"https://{tls_server.base_url}"
    with Session(ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH]) as listing, Session() as other:
        listing.request(url)
        with pytest.raises(RequestError) as caught:
            other.request(url)

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_get_refuses_a_private_certificate(tls_server: RecordingServer) -> None:
    with pytest.raises(mitto.RequestError) as caught:
        mitto.get(f"https://{tls_server.base_url}")

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_an_invalid_spki_hash_is_refused() -> None:
    with pytest.raises(ValueError, match="refused"):
        Session(ignore_certificate_errors_spki_list=["not a hash"])


def test_a_session_reuses_its_connection(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    with Session() as session:
        session.request(url)
        session.request(url)

    first, second = http_server.received
    assert first.client_address == second.client_address


def test_two_sessions_do_not_share_connections(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    with Session() as first_session, Session() as second_session:
        first_session.request(url)
        # The first Session's connection is idle now, so this would use it if the Sessions shared one.
        second_session.request(url)

    first, second = http_server.received
    assert first.client_address != second.client_address


def test_get_reuses_the_connection_of_the_default_session(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    mitto.get(url)
    mitto.get(url)

    first, second = http_server.received
    assert first.client_address == second.client_address


def test_get_does_not_share_a_connection_with_another_session(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    mitto.get(url)
    with Session() as session:
        session.request(url)

    first, second = http_server.received
    assert first.client_address != second.client_address


def test_get_does_not_send_cookies_from_an_earlier_response(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    mitto.get(url)
    mitto.get(url)

    assert "cookie" not in {name.lower() for name, _ in http_server.received[-1].headers}


def test_closing_a_session_closes_its_connections(http_server: RecordingServer) -> None:
    session = Session()
    session.request(f"http://{http_server.base_url}")

    session.close()

    assert http_server.connection_closed.wait(timeout=10)


def test_a_closed_session_refuses_requests(http_server: RecordingServer) -> None:
    session = Session()
    session.close()

    with pytest.raises(RequestError):
        session.request(f"http://{http_server.base_url}")


def test_a_session_runs_requests_from_several_threads_at_once(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    with Session() as session, concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        responses = list(executor.map(session.request, [url] * 32))

    assert [response.status for response in responses] == [200] * 32


def test_headers_arrive_in_the_given_order(http_server: RecordingServer) -> None:
    given = [("Zebra", "1"), ("accept", "text/plain"), ("Alpha", "two words"), ("X-Empty", ""), ("Mango", "caf\xe9")]

    with Session() as session:
        session.request(f"http://{http_server.base_url}", headers=given)

    assert http_server.received[0].headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "keep-alive"),
        *given,
        ("User-Agent", ""),
        ("Accept-Encoding", "gzip, deflate"),
    ]


def test_a_header_that_net_would_add_stays_where_it_was_given(http_server: RecordingServer) -> None:
    given = [
        ("Zebra", "1"),
        ("Accept-Encoding", "identity"),
        ("Connection", "close"),
        ("User-Agent", "test"),
        ("Alpha", "2"),
    ]

    with Session() as session:
        session.request(f"http://{http_server.base_url}", headers=given)

    assert http_server.received[0].headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "close"),
        ("Zebra", "1"),
        ("Accept-Encoding", "identity"),
        ("User-Agent", "test"),
        ("Alpha", "2"),
    ]


def test_a_header_that_cannot_be_encoded_is_refused(http_server: RecordingServer) -> None:
    with Session() as session, pytest.raises(ValueError, match="latin-1"):
        session.request(f"http://{http_server.base_url}", headers=[("X-Name", "€")])


# Frame types and flags from RFC 9113.
HTTP2_PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
HTTP2_HEADERS = 0x1
HTTP2_SETTINGS = 0x4
HTTP2_END_STREAM = 0x1
HTTP2_END_HEADERS = 0x4
HTTP2_PRIORITY = 0x20
# The HPACK encoding of ":status: 200", which is entry 8 of the static table.
HPACK_STATUS_200 = b"\x88"


def http2_frame(frame_type: int, flags: int, stream_id: int, payload: bytes) -> bytes:
    return len(payload).to_bytes(3) + frame_type.to_bytes(1) + flags.to_bytes(1) + stream_id.to_bytes(4) + payload


class Http2Server(socketserver.ThreadingTCPServer):
    """A server that speaks just enough HTTP/2 to answer one request per connection and record its stream weight."""

    daemon_threads = True

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, Http2Handler)
        self.weights: list[int] = []


class Http2Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        assert self.rfile.read(len(HTTP2_PREFACE)) == HTTP2_PREFACE
        self.wfile.write(http2_frame(HTTP2_SETTINGS, 0, 0, b""))

        while True:
            header = self.rfile.read(9)
            size = int.from_bytes(header[:3])
            frame_type, flags = header[3], header[4]
            stream_id = int.from_bytes(header[5:9])
            payload = self.rfile.read(size)
            if frame_type == HTTP2_HEADERS:
                break

        assert flags & HTTP2_PRIORITY
        # The stream dependency takes the first 4 bytes. The next byte is the weight minus one.
        cast(Http2Server, self.server).weights.append(payload[4] + 1)
        self.wfile.write(http2_frame(HTTP2_HEADERS, HTTP2_END_HEADERS | HTTP2_END_STREAM, stream_id, HPACK_STATUS_200))
        # Keeps the connection open until the client closes it.
        self.rfile.read()


@pytest.fixture
def http2_server() -> Iterator[Http2Server]:
    """Run a local HTTP/2 server with the private certificate."""
    context = private_certificate_context()
    context.set_alpn_protocols(["h2"])
    with Http2Server(("127.0.0.1", 0)) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        serve(server)
        yield server
        server.shutdown()


@pytest.mark.parametrize(
    ("priority", "weight"),
    [
        (Priority.THROTTLED, 74),
        (Priority.IDLE, 110),
        (Priority.LOWEST, 147),
        (Priority.LOW, 183),
        (Priority.MEDIUM, 220),
        (Priority.HIGHEST, 256),
    ],
)
def test_the_priority_reaches_the_wire(http2_server: Http2Server, priority: Priority, weight: int) -> None:
    url = f"https://127.0.0.1:{http2_server.server_address[1]}/"

    with Session(ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH]) as session:
        response = session.request(url, priority=priority)

    assert response.protocol == "h2"
    assert http2_server.weights == [weight]
