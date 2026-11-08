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
import select
import socket
import socketserver
import ssl
import subprocess
import sys
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto._native import (
    BodyAvailable,
    Finished,
    NativeEvent,
    NativeRequest,
    Priority,
    RedirectFollowed,
    RequestError,
    ResponseStarted,
    Session,
)
from mitto._persona import CHROME_157_WINDOWS, RequestKind, load_persona
from tests.client_hints import until_a_server_asks
from tests.local_server import LocalHTTPServer
from tools.capture import CASE_HOST, HTTP1, HTTP2, HTTP3, KIND_PATHS, CaptureServer, normal_form
from tools.capture import RequestKind as CaptureRequestKind
from tools.capture.cases import HOST_NAMES
from tools.capture.recording import ConnectionRecord, QuicConnectionRecord

pytestmark = pytest.mark.usefixtures("native_library")

ERR_ABORTED = -3
ERR_INVALID_ARGUMENT = -4
ERR_TOO_MANY_REDIRECTS = -310
ERR_CONTENT_LENGTH_MISMATCH = -354
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


def test_invalid_host_resolver_rules_are_refused() -> None:
    with pytest.raises(ValueError, match="refused"):
        Session(host_resolver_rules="MAP no-address.test")


def test_a_name_that_exists_nowhere_resolves_through_host_resolver_rules(tls_server: RecordingServer) -> None:
    url = f"https://{CASE_HOST}:{tls_server.server_port}/"

    with (
        Session(ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH]) as without_rules,
        pytest.raises(RequestError),
    ):
        without_rules.request(url)
    assert tls_server.received == []

    with Session(
        ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
        host_resolver_rules=f"MAP {CASE_HOST} 127.0.0.1",
    ) as session:
        assert session.request(url).status == 200

    assert dict(tls_server.received[0].headers)["Host"] == f"{CASE_HOST}:{tls_server.server_port}"


def sent_server_name(connection: ConnectionRecord | QuicConnectionRecord) -> str:
    """Return the name in a connection's SNI, which is one list entry of one name."""
    assert connection.client_hello is not None
    extension = next(extension for extension in connection.client_hello.hello.extensions if extension.type == 0)
    return extension.data[5:].decode()


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_host_resolver_rules_reach_every_name_and_leave_it_in_sni(protocol: str) -> None:
    path = KIND_PATHS[CaptureRequestKind.FETCH]
    with (
        CaptureServer() as server,
        Session(
            ignore_certificate_errors_spki_list=[server.spki_hash],
            origins_to_force_quic_on=[server.authority(HTTP3, host=name) for name in HOST_NAMES]
            if protocol == HTTP3
            else [],
            host_resolver_rules=server.host_resolver_rules,
        ) as session,
    ):
        for name in HOST_NAMES:
            assert session.request(server.url(protocol, host=name, path=path)).status == 200
        connections = server.recording(protocol).connections

    assert sent_server_name(connections[0]) == CASE_HOST


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


def test_a_cookie_goes_after_what_net_adds_of_its_own_over_http1(http_server: RecordingServer) -> None:
    url = f"http://{http_server.base_url}"

    with Session(accept_language="en") as session:
        session.request(url)
        session.request(url)

    assert [name for name, _ in http_server.received[-1].headers][-3:] == [
        "Accept-Encoding",
        "Accept-Language",
        "Cookie",
    ]


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


def test_the_user_agent_setting_is_what_net_adds_in_the_same_place(http_server: RecordingServer) -> None:
    given = [("Zebra", "1"), ("Alpha", "2")]

    with Session(user_agent="test agent") as session:
        session.request(f"http://{http_server.base_url}", headers=given)

    assert http_server.received[0].headers == [
        ("Host", http_server.base_url.rstrip("/")),
        ("Connection", "keep-alive"),
        *given,
        ("User-Agent", "test agent"),
        ACCEPT_ENCODING,
    ]


def test_a_user_agent_among_the_headers_replaces_the_setting(http_server: RecordingServer) -> None:
    with Session(user_agent="test agent") as session:
        session.request(f"http://{http_server.base_url}", headers=[("User-Agent", "given")])

    assert [value for name, value in http_server.received[0].headers if name == "User-Agent"] == ["given"]


def test_an_invalid_user_agent_is_refused() -> None:
    with pytest.raises(ValueError, match="refused the settings"):
        Session(user_agent="a\r\nX-Injected: b")


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
        *[
            line
            for line in until_a_server_asks(persona.request_defaults(RequestKind.NAVIGATION)["headers"])
            if line[0] != "Origin"
        ],
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


# How long a test waits for one thing to happen, so that a bug fails the test and does not hang it.
EVENT_TIMEOUT_SECONDS = 30.0

# Larger by far than what the socket buffers of a local connection hold on either Host, so that a body that nobody reads
# has to stall.
LARGE_BODY_SIZE = 64 * 1024 * 1024
LARGE_BODY_CHUNK = bytes(64 * 1024)

# How long a server has to send nothing to count as stalled. Too short only makes a test prove less.
STALL_SECONDS = 0.5


class ScriptedServer(LocalHTTPServer):
    """An HTTP/1.1 server whose paths behave in the ways that a request's events have to cope with."""

    request_queue_size = socket.SOMAXCONN

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, ScriptedHandler)
        self.requests_seen = threading.Semaphore(0)
        self.methods: list[str] = []
        self.stop_holding = threading.Event()
        self.bytes_sent = 0
        self.large_body_sent = threading.Event()

    def url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.server_port}{path}"

    def wait_for_requests(self, count: int) -> None:
        for _ in range(count):
            assert self.requests_seen.acquire(timeout=EVENT_TIMEOUT_SECONDS)

    def wait_until_stalled(self) -> int:
        """Return how many bytes of the large body have gone out, once that has stopped growing."""
        deadline = time.monotonic() + EVENT_TIMEOUT_SECONDS
        previous = -1
        while previous != self.bytes_sent or previous == 0:
            assert time.monotonic() < deadline
            previous = self.bytes_sent
            time.sleep(STALL_SECONDS)
        return previous


class ScriptedHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast(ScriptedServer, self.server)
        server.methods.append(self.command)
        server.requests_seen.release()
        match self.path:
            case "/hold":
                # The timeout only stops a thread from outliving a test that failed.
                server.stop_holding.wait(timeout=EVENT_TIMEOUT_SECONDS * 2)
                self.send_small_body()
            case "/large":
                self.send_large_body()
            case "/redirect":
                self.send_redirect("/small")
            case "/loop":
                self.send_redirect("/loop")
            case "/truncated":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(b"too short")
            case _:
                self.send_small_body()

    def do_HEAD(self) -> None:
        self.do_GET()

    def do_PUT(self) -> None:
        self.do_GET()

    def send_small_body(self) -> None:
        self.send_response(200)
        self.send_header("Content-Length", "5")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(b"small")

    def send_redirect(self, location: str) -> None:
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def send_large_body(self) -> None:
        server = cast(ScriptedServer, self.server)
        self.send_response(200)
        self.send_header("Content-Length", str(LARGE_BODY_SIZE))
        self.end_headers()
        remaining = LARGE_BODY_SIZE
        while remaining > 0:
            self.wfile.write(LARGE_BODY_CHUNK)
            remaining -= len(LARGE_BODY_CHUNK)
            server.bytes_sent += len(LARGE_BODY_CHUNK)
        server.large_body_sent.set()

    def handle(self) -> None:
        # A client that cancels goes away in the middle of what this sends.
        try:
            super().handle()
        except ConnectionError:
            self.close_connection = True

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def scripted_server() -> Iterator[ScriptedServer]:
    """Run the local server with the paths above."""
    with ScriptedServer(("127.0.0.1", 0)) as server:
        serve(server)
        yield server
        server.stop_holding.set()
        server.shutdown()


def next_event(request: NativeRequest) -> NativeEvent:
    event = request.next_event(EVENT_TIMEOUT_SECONDS)
    assert event is not None
    return event


def read_available(request: NativeRequest) -> int:
    """Read what has arrived, which asks for more once that is gone, and return how many bytes came."""
    size = 0
    while chunk := request.read():
        size += len(chunk)
    return size


def run_to_end(request: NativeRequest) -> tuple[list[NativeEvent], int]:
    """Read the whole body, and return the events on the way and the size of the body."""
    events: list[NativeEvent] = []
    size = 0
    while True:
        event = next_event(request)
        events.append(event)
        if isinstance(event, Finished):
            return events, size
        if isinstance(event, ResponseStarted | BodyAvailable):
            size += read_available(request)


def finished_net_error(request: NativeRequest) -> int:
    """Take events until the last one, without reading, and return its net error."""
    while True:
        event = next_event(request)
        if isinstance(event, Finished):
            return event.net_error


def is_readable(fd: int, timeout: float = 0) -> bool:
    return bool(select.select([fd], [], [], timeout)[0])


def test_a_response_arrives_as_events(scripted_server: ScriptedServer) -> None:
    url = scripted_server.url("/small")

    with Session() as session, session.start(url) as request:
        started = next_event(request)
        assert isinstance(started, ResponseStarted)
        assert started.status == 200
        assert started.protocol == "http/1.1"
        assert started.url == url
        assert ("Content-Length", "5") in started.headers

        assert request.read() is None
        assert isinstance(next_event(request), BodyAvailable)
        assert request.read() == b"small"

        assert request.read() is None
        assert next_event(request) == Finished(net_error=0)
        assert request.read() == b""


def test_a_body_can_be_read_in_small_pieces(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/small")) as request:
        assert isinstance(next_event(request), ResponseStarted)
        assert request.read(2) is None
        assert isinstance(next_event(request), BodyAvailable)

        assert request.read(2) == b"sm"
        assert request.read(2) == b"al"
        assert request.read(2) == b"l"


def test_a_read_before_the_response_starts_the_body_when_it_can(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/small")) as request:
        assert request.read() is None

        assert isinstance(next_event(request), ResponseStarted)
        assert isinstance(next_event(request), BodyAvailable)
        assert request.read() == b"small"


def test_a_read_of_nothing_is_refused(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/small")) as request:
        for size in [0, -1]:
            with pytest.raises(RequestError) as caught:
                request.read(size)
            assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_redirect_arrives_as_an_event(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/redirect")) as request:
        events, size = run_to_end(request)

    redirect, started, *_ = events
    assert isinstance(redirect, RedirectFollowed)
    assert redirect.status == 302
    assert redirect.url == scripted_server.url("/redirect")
    assert ("Location", "/small") in redirect.headers
    assert isinstance(started, ResponseStarted)
    assert started.url == scripted_server.url("/small")
    assert size == len(b"small")


def test_a_blocking_request_ends_at_the_final_url(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        response = session.request(scripted_server.url("/redirect"))

    assert response.url == scripted_server.url("/small")
    assert response.body == b"small"
    assert [hop.url for hop in response.history] == [scripted_server.url("/redirect")]


def test_a_redirect_that_is_not_followed_starts_and_finishes(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/redirect"), follow_redirects=False) as request:
        events, size = run_to_end(request)

    started, finished = events
    assert isinstance(started, ResponseStarted)
    assert started.status == 302
    assert started.url == scripted_server.url("/redirect")
    assert finished == Finished(net_error=0)
    assert size == 0


def test_an_option_that_is_not_a_boolean_is_refused(scripted_server: ScriptedServer) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.start(scripted_server.url("/small"), follow_redirects=cast(bool, "no"))
    assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_too_many_redirects_finish_with_the_net_error(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/loop")) as request:
        events, _ = run_to_end(request)

    assert events[-1] == Finished(net_error=ERR_TOO_MANY_REDIRECTS)
    assert all(isinstance(event, RedirectFollowed) for event in events[:-1])


def test_a_body_that_ends_early_finishes_with_the_net_error(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/truncated")) as request:
        events, size = run_to_end(request)

    assert events[-1] == Finished(net_error=ERR_CONTENT_LENGTH_MISMATCH)
    assert size == len(b"too short")


def test_a_blocking_request_raises_when_the_body_ends_early(scripted_server: ScriptedServer) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.request(scripted_server.url("/truncated"))

    assert caught.value.net_error == ERR_CONTENT_LENGTH_MISMATCH


def test_next_event_returns_none_when_none_arrives_in_time(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/hold")) as request:
        scripted_server.wait_for_requests(1)

        assert request.next_event(0) is None
        assert request.next_event(0.01) is None


def test_next_event_raises_after_the_last_event(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start("not a url") as request:
        assert isinstance(next_event(request), Finished)

        with pytest.raises(RequestError):
            request.next_event()


def test_a_closed_request_refuses_what_needs_it(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        request = session.start(scripted_server.url("/small"))
        request.close()

        with pytest.raises(RequestError):
            request.next_event()
        with pytest.raises(RequestError):
            request.read()
        request.cancel()
        request.close()


@pytest.mark.parametrize("method", ["HEAD", "PUT"])
def test_the_method_reaches_the_wire(scripted_server: ScriptedServer, method: str) -> None:
    with Session() as session:
        response = session.request(scripted_server.url("/small"), method=method)

    assert response.status == 200
    assert scripted_server.methods == [method]


@pytest.mark.parametrize("method", ["CONNECT", "TRACE", "track", "no spaces", ""])
def test_a_method_chrome_would_not_send_is_refused(scripted_server: ScriptedServer, method: str) -> None:
    with Session() as session, pytest.raises(RequestError) as caught:
        session.request(scripted_server.url("/small"), method=method)

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert str(caught.value).startswith(method)
    assert scripted_server.methods == []


def test_a_server_sending_a_large_body_stalls_until_the_caller_reads(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/large")) as request:
        assert isinstance(next_event(request), ResponseStarted)

        assert scripted_server.wait_until_stalled() < LARGE_BODY_SIZE
        assert not scripted_server.large_body_sent.is_set()

        assert request.read() is None
        events, size = run_to_end(request)

    assert events[-1] == Finished(net_error=0)
    assert size == LARGE_BODY_SIZE
    assert scripted_server.large_body_sent.wait(timeout=EVENT_TIMEOUT_SECONDS)


def test_a_request_is_cancelled_before_its_response(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        with (
            session.start(scripted_server.url("/hold")) as other,
            session.start(scripted_server.url("/hold")) as cancelled,
        ):
            scripted_server.wait_for_requests(2)

            cancelled.cancel()

            assert next_event(cancelled) == Finished(net_error=ERR_ABORTED)
            assert other.next_event(0) is None
            scripted_server.stop_holding.set()
            events, size = run_to_end(other)
            assert events[-1] == Finished(net_error=0)
            assert size == len(b"small")

        assert session.request(scripted_server.url("/small")).body == b"small"


def test_a_request_is_cancelled_in_the_middle_of_its_body(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        with (
            session.start(scripted_server.url("/large")) as other,
            session.start(scripted_server.url("/large")) as cancelled,
        ):
            assert isinstance(next_event(cancelled), ResponseStarted)
            assert cancelled.read() is None
            assert isinstance(next_event(cancelled), BodyAvailable)
            assert cancelled.read()

            cancelled.cancel()

            assert finished_net_error(cancelled) == ERR_ABORTED
            events, size = run_to_end(other)
            assert events[-1] == Finished(net_error=0)
            assert size == LARGE_BODY_SIZE

        assert session.request(scripted_server.url("/small")).body == b"small"


def test_cancelling_a_finished_request_changes_nothing(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/small")) as request:
        events, _ = run_to_end(request)

        request.cancel()

        assert events[-1] == Finished(net_error=0)
        assert request.read() == b""


def test_a_cancelled_request_can_read_what_had_arrived(scripted_server: ScriptedServer) -> None:
    with Session() as session, session.start(scripted_server.url("/small")) as request:
        assert request.read() is None
        assert isinstance(next_event(request), ResponseStarted)
        assert isinstance(next_event(request), BodyAvailable)

        request.cancel()

        assert next_event(request) == Finished(net_error=ERR_ABORTED)
        assert request.read() == b"small"
        assert request.read() == b""


def test_cancel_ends_a_wait_on_another_thread(scripted_server: ScriptedServer) -> None:
    with (
        Session() as session,
        session.start(scripted_server.url("/hold")) as request,
        concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor,
    ):
        scripted_server.wait_for_requests(1)
        waiting = executor.submit(request.next_event)

        request.cancel()

        assert waiting.result(timeout=EVENT_TIMEOUT_SECONDS) == Finished(net_error=ERR_ABORTED)


def test_a_request_that_is_closed_running_leaves_the_session_working(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        session.start(scripted_server.url("/large")).close()

        assert session.request(scripted_server.url("/small")).body == b"small"


def test_closing_a_session_ends_its_running_requests(scripted_server: ScriptedServer) -> None:
    session = Session()
    held = session.start(scripted_server.url("/hold"))
    with session.start(scripted_server.url("/large")) as large:
        assert isinstance(next_event(large), ResponseStarted)
        scripted_server.wait_for_requests(2)

        session.close()

        assert finished_net_error(large) == ERR_ABORTED
    assert next_event(held) == Finished(net_error=ERR_ABORTED)
    held.close()


def test_closing_a_session_ends_a_blocking_request_on_another_thread(scripted_server: ScriptedServer) -> None:
    session = Session()
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        blocking = executor.submit(session.request, scripted_server.url("/hold"))
        scripted_server.wait_for_requests(1)

        session.close()

        error = blocking.exception(timeout=EVENT_TIMEOUT_SECONDS)
    assert isinstance(error, RequestError)
    assert error.net_error == ERR_ABORTED


@pytest.mark.skipif(sys.platform == "win32", reason="a Session has no file descriptor on Windows")
def test_the_event_fd_is_readable_while_an_event_is_waiting(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        fd = session.event_fd()
        assert session.event_fd() == fd
        assert not is_readable(fd)
        assert session.next_ready_request() is None

        with session.start(scripted_server.url("/small")) as request:
            assert is_readable(fd, timeout=EVENT_TIMEOUT_SECONDS)
            assert session.next_ready_request() == request.identity
            assert not is_readable(fd)
            assert isinstance(request.next_event(0), ResponseStarted)
            assert session.next_ready_request() is None

            # The body waits to be asked for, so nothing else can arrive yet.
            assert not is_readable(fd, timeout=0.05)
            assert request.read() is None

            assert is_readable(fd, timeout=EVENT_TIMEOUT_SECONDS)
            assert session.next_ready_request() == request.identity
            assert isinstance(request.next_event(0), BodyAvailable)
            assert request.read() == b"small"
            assert request.read() is None

            assert is_readable(fd, timeout=EVENT_TIMEOUT_SECONDS)
            assert session.next_ready_request() == request.identity
            assert request.next_event(0) == Finished(net_error=0)
            assert not is_readable(fd)


@pytest.mark.skipif(sys.platform == "win32", reason="a Session has no file descriptor on Windows")
def test_the_event_fd_serves_all_the_requests_of_a_session(scripted_server: ScriptedServer) -> None:
    url = scripted_server.url("/small")

    with Session() as session:
        fd = session.event_fd()
        requests = {request.identity: request for request in [session.start(url) for _ in range(8)]}
        remaining = set(requests)
        while remaining:
            assert is_readable(fd, timeout=EVENT_TIMEOUT_SECONDS)
            identity = session.next_ready_request()
            assert identity is not None
            event = requests[identity].next_event(0)
            if isinstance(event, ResponseStarted | BodyAvailable):
                read_available(requests[identity])
            elif isinstance(event, Finished):
                assert event.net_error == 0
                remaining.remove(identity)
        for request in requests.values():
            request.close()


@pytest.mark.skipif(sys.platform == "win32", reason="a Session has no file descriptor on Windows")
def test_a_request_closed_with_an_event_waiting_can_still_come_up(scripted_server: ScriptedServer) -> None:
    with Session() as session:
        fd = session.event_fd()
        request = session.start(scripted_server.url("/small"))
        identity = request.identity
        assert is_readable(fd, timeout=EVENT_TIMEOUT_SECONDS)

        request.close()

        assert session.next_ready_request() == identity
        assert session.request(scripted_server.url("/small")).body == b"small"


def test_a_wait_returns_once_each_time_the_list_fills(scripted_server: ScriptedServer) -> None:
    url = scripted_server.url("/small")

    with Session() as session:
        # The first wait starts the list.
        assert not session.wait_for_event(0)
        with session.start(url) as first, session.start(url) as second:
            assert session.wait_for_event(EVENT_TIMEOUT_SECONDS)
            assert isinstance(next_event(first), ResponseStarted)
            assert isinstance(next_event(second), ResponseStarted)

            # Both events are listed now, and the wait has returned for the list as it is.
            assert not session.wait_for_event(0.05)
            assert {session.next_ready_request(), session.next_ready_request()} == {first.identity, second.identity}
            assert session.next_ready_request() is None

            # The list is empty, so the next event is news.
            assert first.read() is None
            assert session.wait_for_event(EVENT_TIMEOUT_SECONDS)
            assert session.next_ready_request() == first.identity
            assert isinstance(next_event(first), BodyAvailable)


def test_detaching_a_session_ends_a_wait_on_another_thread() -> None:
    session = Session()
    assert not session.wait_for_event(0)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        waiting = executor.submit(session.wait_for_event)
        assert not waiting.done()

        destroy = session.detach()

        assert waiting.result(timeout=EVENT_TIMEOUT_SECONDS) is False
    assert session.wait_for_event() is False
    destroy()


# Each of these starts a request and exits with it running. Each waits to be told to exit, once the request has reached
# the server. What the test wants to see is how the interpreter's shutdown and the network thread get along: a thread
# that is waiting for an event when Python goes away must not stop the network thread or the process.
EXIT_PROGRAMS = {
    "a-body-that-nobody-reads": """
import sys
from mitto._native import ResponseStarted, Session

request = Session().start(sys.argv[1])
while not isinstance(request.next_event(), ResponseStarted):
    pass
sys.stdin.readline()
""",
    "a-thread-blocked-in-a-request": """
import sys
import threading
from mitto._native import Session

threading.Thread(target=Session().request, args=(sys.argv[1],), daemon=True).start()
sys.stdin.readline()
""",
    "a-thread-in-the-middle-of-reading": """
import sys
import threading
from mitto._native import Session

threading.Thread(target=Session().request, args=(sys.argv[1],), daemon=True).start()
sys.stdin.readline()
""",
    "several-requests": """
import sys
from mitto._native import Session

session = Session()
requests = [session.start(sys.argv[1]) for _ in range(8)]
sys.stdin.readline()
""",
}

EXIT_PROGRAM_PATHS = {
    "a-body-that-nobody-reads": "/large",
    "a-thread-blocked-in-a-request": "/hold",
    "a-thread-in-the-middle-of-reading": "/large",
    "several-requests": "/hold",
}


@pytest.mark.parametrize("name", EXIT_PROGRAMS)
def test_a_process_exits_cleanly_with_a_request_running(scripted_server: ScriptedServer, name: str) -> None:
    process = subprocess.Popen(
        [sys.executable, "-c", EXIT_PROGRAMS[name], scripted_server.url(EXIT_PROGRAM_PATHS[name])],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        scripted_server.wait_for_requests(1)
        # So that a response arrives while the process is on its way out.
        scripted_server.stop_holding.set()
        stdout, stderr = process.communicate("\n", timeout=EVENT_TIMEOUT_SECONDS)
    finally:
        process.kill()
        process.wait()

    assert process.returncode == 0
    assert stdout == ""
    assert stderr == ""


HTTP2_RST_STREAM = 0x3


class MultiplexingHttp2Server(socketserver.ThreadingTCPServer):
    """A server that speaks just enough HTTP/2 to hold a connection's streams open until the client resets one."""

    daemon_threads = True

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, MultiplexingHttp2Handler)
        self.connections = 0
        self.hold_streams = False
        self.streams_held = threading.Semaphore(0)
        self.resets: list[int] = []


class MultiplexingHttp2Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        server = cast(MultiplexingHttp2Server, self.server)
        if self.rfile.read(len(HTTP2_PREFACE)) != HTTP2_PREFACE:
            return
        server.connections += 1
        self.wfile.write(http2_frame(HTTP2_SETTINGS, 0, 0, b""))

        held: list[int] = []
        while len(header := self.rfile.read(9)) == 9:
            frame_type = header[3]
            stream_id = int.from_bytes(header[5:9])
            self.rfile.read(int.from_bytes(header[:3]))
            if frame_type == HTTP2_HEADERS and server.hold_streams:
                held.append(stream_id)
                server.streams_held.release()
            elif frame_type == HTTP2_HEADERS:
                self.answer(stream_id)
            elif frame_type == HTTP2_RST_STREAM:
                server.resets.append(stream_id)
                held.remove(stream_id)
                for other in held:
                    self.answer(other)

    def answer(self, stream_id: int) -> None:
        self.wfile.write(http2_frame(HTTP2_HEADERS, HTTP2_END_HEADERS | HTTP2_END_STREAM, stream_id, HPACK_STATUS_200))


def test_cancelling_a_stream_leaves_the_others_on_its_connection() -> None:
    context = private_certificate_context()
    context.set_alpn_protocols(["h2"])
    with MultiplexingHttp2Server(("127.0.0.1", 0)) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        serve(server)
        url = f"https://127.0.0.1:{server.server_address[1]}/"
        with Session(ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH]) as session:
            # Without a connection to share, the two requests below could each open one.
            assert session.request(url).protocol == "h2"
            server.hold_streams = True
            with session.start(url) as cancelled, session.start(url) as other:
                for _ in range(2):
                    assert server.streams_held.acquire(timeout=EVENT_TIMEOUT_SECONDS)

                cancelled.cancel()

                assert next_event(cancelled) == Finished(net_error=ERR_ABORTED)
                started = next_event(other)
                assert isinstance(started, ResponseStarted)
                assert started.status == 200
                assert started.protocol == "h2"
        server.shutdown()

    assert server.connections == 1
    assert len(server.resets) == 1
