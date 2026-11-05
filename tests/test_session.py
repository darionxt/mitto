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
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto._native import Priority, RequestError, Session
from mitto._persona import CHROME_157_WINDOWS, RequestKind, load_persona
from tests.local_server import LocalHTTPServer
from tools.capture import HTTP2, HTTP3, KIND_PATHS, CaptureServer, normal_form
from tools.capture import RequestKind as CaptureRequestKind

pytestmark = pytest.mark.usefixtures("native_library")

ERR_INVALID_ARGUMENT = -4
ERR_CERT_AUTHORITY_INVALID = -202

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="

# How long a Session may take to move to HTTP/3 once Alt-Svc has advertised it. A slow CI runner needs more than a local
# handshake does.
ALT_SVC_TIMEOUT_SECONDS = 30.0

# A valid hash of some other key: the Base64 form of 32 bytes.
OTHER_SPKI_HASH = "A" * 43 + "="

# What //net advertises over HTTPS and to localhost, which is where the test servers are.
ACCEPT_ENCODING = ("Accept-Encoding", "gzip, deflate, br, zstd")

# The word "hello" in each encoding. The Brotli stream is a metablock of uncompressed data. The Zstandard frame is one
# raw block, with no checksum.
ENCODED_HELLO = {
    "br": bytes.fromhex("0b028068656c6c6f03"),
    "zstd": bytes.fromhex("28b52ffd200529000068656c6c6f"),
}


@dataclass(frozen=True, slots=True)
class ReceivedRequest:
    """What a server saw of one request."""

    client_address: tuple[str, int]
    headers: list[tuple[str, str]]


class RecordingServer(LocalHTTPServer):
    """An HTTP/1.1 server that keeps connections open and records each request."""

    # A Session opens up to six connections to a host at once, and the default queue holds five. macOS makes room in a
    # full queue by dropping a half-open connection, which the client sees as a reset.
    request_queue_size = socket.SOMAXCONN

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, RecordingHandler)
        self.received: list[ReceivedRequest] = []
        self.response_headers: list[tuple[str, str]] = []
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
        for name, value in server.response_headers:
            self.send_header(name, value)
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


def test_an_invalid_origin_to_force_quic_on_is_refused() -> None:
    with pytest.raises(ValueError, match="refused"):
        Session(origins_to_force_quic_on=["no port"])


def test_a_session_moves_to_http3_through_alt_svc(tls_server: RecordingServer) -> None:
    with CaptureServer() as quic_server:
        tls_server.response_headers = [("Alt-Svc", f'h3=":{quic_server.port(HTTP3)}"')]
        # The origin here is the TCP one, so this forces nothing. It only lifts //net's refusal of QUIC to a private
        # certificate for the host, which a public server does not need.
        with Session(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH, quic_server.spki_hash],
            origins_to_force_quic_on=[quic_server.authority(HTTP3)],
        ) as session:
            url = f"https://{quic_server.host}:{tls_server.server_port}{KIND_PATHS[CaptureRequestKind.FETCH]}"
            first = session.request(url)
            # //net starts QUIC beside a connection that can serve the request at once, so a later request gets h3 once
            # the handshake has finished.
            deadline = time.monotonic() + ALT_SVC_TIMEOUT_SECONDS
            protocol = session.request(url).protocol
            while protocol != "h3" and time.monotonic() < deadline:
                time.sleep(0.01)
                protocol = session.request(url).protocol

    assert first.protocol == "http/1.1"
    assert protocol == "h3"


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
        ACCEPT_ENCODING,
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


def test_the_accept_language_setting_comes_after_what_else_net_adds(http_server: RecordingServer) -> None:
    with Session(accept_language="en-GB,en;q=0.9") as session:
        session.request(f"http://{http_server.base_url}", headers=[("User-Agent", "test")])

    assert http_server.received[0].headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "keep-alive"),
        ("User-Agent", "test"),
        ACCEPT_ENCODING,
        ("Accept-Language", "en-GB,en;q=0.9"),
    ]


def test_an_accept_language_among_the_headers_replaces_the_setting(http_server: RecordingServer) -> None:
    with Session(accept_language="en-GB") as session:
        session.request(f"http://{http_server.base_url}", headers=[("Accept-Language", "fr")])

    assert ("Accept-Language", "fr") in http_server.received[0].headers
    assert ("Accept-Language", "en-GB") not in http_server.received[0].headers


def test_an_invalid_accept_language_is_refused() -> None:
    with pytest.raises(ValueError, match="refused"):
        Session(accept_language="a\r\nX-Injected: b")


def test_the_referrer_comes_after_the_headers_and_before_what_net_adds(http_server: RecordingServer) -> None:
    referrer = f"http://{http_server.base_url}page"

    with Session(accept_language="en") as session:
        session.request(f"http://{http_server.base_url}", headers=[("User-Agent", "test")], referrer=referrer)

    assert http_server.received[0].headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "keep-alive"),
        ("User-Agent", "test"),
        ("Referer", referrer),
        ACCEPT_ENCODING,
        ("Accept-Language", "en"),
    ]


def test_the_referrer_leaves_out_a_username_a_password_and_a_fragment(http_server: RecordingServer) -> None:
    with Session() as session:
        session.request(
            f"http://{http_server.base_url}", referrer=f"http://user:secret@{http_server.base_url}page#fragment"
        )

    assert ("Referer", f"http://{http_server.base_url}page") in http_server.received[0].headers


def test_a_referer_among_the_headers_is_refused(http_server: RecordingServer) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.request(f"http://{http_server.base_url}", headers=[("referer", "http://127.0.0.1/")])

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert http_server.received == []


def test_a_referrer_that_net_cannot_send_is_refused(http_server: RecordingServer) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.request(f"http://{http_server.base_url}", referrer="not a url")

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert http_server.received == []


def test_a_referrer_with_a_nul_is_refused(http_server: RecordingServer) -> None:
    with Session() as session, pytest.raises(ValueError, match="NUL"):
        session.request(f"http://{http_server.base_url}", referrer="http://127.0.0.1/\0")


class EncodedHelloHandler(http.server.BaseHTTPRequestHandler):
    """Answers /br and /zstd with the word "hello" in that encoding."""

    def do_GET(self) -> None:
        encoding = self.path.removeprefix("/")
        self.send_response(200)
        self.send_header("Content-Encoding", encoding)
        self.send_header("Content-Length", str(len(ENCODED_HELLO[encoding])))
        self.end_headers()
        self.wfile.write(ENCODED_HELLO[encoding])

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.mark.parametrize("encoding", ENCODED_HELLO)
def test_a_session_decodes_what_it_advertises(encoding: str) -> None:
    with LocalHTTPServer(("127.0.0.1", 0), EncodedHelloHandler) as server, Session() as session:
        serve(server)
        response = session.request(f"http://127.0.0.1:{server.server_port}/{encoding}")
        server.shutdown()

    assert response.body == b"hello"


def test_get_presents_chrome_157_with_the_navigation_request_kind(http_server: RecordingServer) -> None:
    persona = load_persona(CHROME_157_WINDOWS)

    mitto.get(f"http://{http_server.base_url}")

    headers = http_server.received[0].headers
    assert headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "keep-alive"),
        *persona.request_defaults(RequestKind.NAVIGATION, "http://127.0.0.1/")["headers"],
        ACCEPT_ENCODING,
        ("Accept-Language", persona.accept_language),
    ]
    assert ("Sec-Fetch-Mode", "navigate") in headers
    assert dict(headers)["User-Agent"].endswith("Chrome/157.0.0.0 Safari/537.36")


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


@pytest.mark.parametrize(
    ("priority", "priority_incremental", "line"),
    [
        (Priority.HIGHEST, True, "priority: u=0, i"),
        (Priority.HIGHEST, False, "priority: u=0"),
        (Priority.MEDIUM, True, "priority: u=1, i"),
        (Priority.MEDIUM, False, "priority: u=1"),
        (Priority.LOWEST, True, "priority: i"),
    ],
)
def test_the_priority_and_whether_it_is_incremental_reach_the_priority_header(
    priority: Priority, priority_incremental: bool, line: str
) -> None:
    kind = CaptureRequestKind.FETCH

    with CaptureServer() as server, Session(ignore_certificate_errors_spki_list=[server.spki_hash]) as session:
        session.request(
            f"https://{server.authority(HTTP2)}{KIND_PATHS[kind]}",
            priority=priority,
            priority_incremental=priority_incremental,
        )

    form = cast(dict[str, Any], normal_form(server.recording(HTTP2)))
    assert line in form["requests"][kind.value]["headers"]
