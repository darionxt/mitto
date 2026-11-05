"""Tests of the capture server through real sockets and real TLS, with Python's own TLS as the client."""

import base64
import errno
import hashlib
import json
import logging
import re
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from hpack import Decoder, Encoder

from tests.capture import samples
from tools.capture import DEFAULT_HOST, HTTP1, HTTP2, KIND_PATHS, CaptureServer, RequestKind, normal_form
from tools.capture.http2 import DATA, END_STREAM, HEADERS, PREFACE, SETTINGS, FrameParser, serialize_frame
from tools.capture.page import DONE_PATH
from tools.capture.recording import ConnectionRecord

_REPOSITORY = Path(__file__).parents[2]
_TIMEOUT_SECONDS = 10.0


def client_context(offered: tuple[str, ...] = ("h2", "http/1.1")) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.set_alpn_protocols(list(offered))
    return context


def connect(port: int, offered: tuple[str, ...] = ("h2", "http/1.1"), host: str = DEFAULT_HOST) -> ssl.SSLSocket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=_TIMEOUT_SECONDS)
    return client_context(offered).wrap_socket(sock, server_hostname=host)


def http1_get(sock: ssl.SSLSocket, target: str) -> bytes:
    """Send a GET and return the whole response."""
    sock.sendall(f"GET {target} HTTP/1.1\r\nHost: {sock.server_hostname}\r\nAccept: */*\r\n\r\n".encode())
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


class Http2Client:
    """Just enough of an HTTP/2 client to send GETs and read the answers."""

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
        status = b""
        body = bytearray()
        while True:
            for frame in self.parser.feed(self.sock.recv(1 << 16)):
                if frame.type == HEADERS:
                    status = dict(self.decoder.decode(frame.payload, raw=True))[b":status"]
                elif frame.type == DATA and frame.stream_id == stream_id:
                    body += frame.payload
                    if frame.flags & END_STREAM:
                        return status, bytes(body)


def wait_for_ended_connection(server: CaptureServer, protocol: str) -> ConnectionRecord:
    """Wait until the first connection on a port has ended in an error, which a real client's close does."""
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        connections = server.recording(protocol).connections
        if connections and connections[0].error is not None:
            return connections[0]
        time.sleep(0.01)
    raise AssertionError("the connection did not end")


def test_a_host_must_be_a_localhost_name() -> None:
    with pytest.raises(ValueError, match="localhost"):
        CaptureServer("example.com")


def test_prints_the_hash_of_the_certificate_it_serves() -> None:
    with CaptureServer() as server, connect(server.port(HTTP1)) as sock:
        der = sock.getpeercert(binary_form=True)
        assert der is not None
        certificate = x509.load_der_x509_certificate(der)
        spki = certificate.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
        )
        assert server.spki_hash == base64.b64encode(hashlib.sha256(spki).digest()).decode()
        names = certificate.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        assert names.get_values_for_type(x509.DNSName) == [server.host]


def test_each_port_chooses_its_protocol_through_alpn() -> None:
    with CaptureServer() as server, connect(server.port(HTTP1)) as first, connect(server.port(HTTP2)) as second:
        assert server.port(HTTP1) != server.port(HTTP2)
        assert first.selected_alpn_protocol() == "http/1.1"
        assert second.selected_alpn_protocol() == "h2"


def test_a_client_without_h2_gets_http1_on_the_http2_port() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP2), offered=("http/1.1",)) as sock:
            assert sock.selected_alpn_protocol() is None
            assert http1_get(sock, "/fetch").startswith(b"HTTP/1.1 200")
        assert normal_form(server.recording(HTTP2))["protocol"] == "http/1.1"


def test_records_http1() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP1)) as sock:
            response = http1_get(sock, "/fetch")
            http1_get(sock, "/missing")
        assert response.endswith(b"\r\n\r\nfetched\n")
        recording = server.recording(HTTP1)

    assert recording.authority == server.authority(HTTP1)
    connection = recording.connections[0]
    assert connection.alpn == "http/1.1"
    assert connection.tls_version is not None
    assert connection.cipher is not None
    assert [request.target for request in connection.requests] == ["/fetch", "/missing"]
    assert normal_form(recording)["requests"] == {
        "fetch": {"request_line": "GET /fetch HTTP/1.1", "headers": ["Host: capture.localhost", "Accept: */*"]}
    }


def test_records_http2() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP2)) as sock:
            client = Http2Client(sock)
            assert client.get("/fetch") == (b"200", b"fetched\n")
            assert client.get("/missing") == (b"404", b"not found\n")
        recording = server.recording(HTTP2)

    connection = recording.connections[0]
    assert connection.alpn == "h2"
    assert connection.frames[0].type == SETTINGS
    assert [request.target for request in connection.requests] == ["/fetch", "/missing"]
    form = normal_form(recording)
    assert form["protocol"] == "h2"
    assert form["http2"] == {"preface": [{"type": "SETTINGS", "settings": ["INITIAL_WINDOW_SIZE: 65536"]}]}


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_the_test_page_ends_with_the_done_path(protocol: str) -> None:
    with CaptureServer() as server, connect(server.port(protocol)) as sock:
        client = Http2Client(sock) if protocol == HTTP2 else None

        def get(target: str) -> None:
            if client is not None:
                client.get(target)
            else:
                http1_get(sock, target)

        for kind in RequestKind:
            get(KIND_PATHS[kind])
        assert not server.wait_for_page(protocol, 0)
        other = HTTP1 if protocol == HTTP2 else HTTP2
        get(DONE_PATH)
        assert server.wait_for_page(protocol, _TIMEOUT_SECONDS)
        assert not server.wait_for_page(other, 0)

        requests = normal_form(server.recording(protocol))["requests"]
        assert isinstance(requests, dict)
        assert list(requests) == ["navigation", "fetch", "script", "image"]


def test_connections_on_a_port_are_recorded_separately_from_the_other_port() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP1)) as sock:
            http1_get(sock, "/")
        assert len(server.recording(HTTP1).connections) == 1
        assert server.recording(HTTP2).connections == ()


def test_gives_no_session_tickets() -> None:
    context = client_context()
    with CaptureServer() as server:
        address = ("127.0.0.1", server.port(HTTP1))
        with (
            socket.create_connection(address, timeout=_TIMEOUT_SECONDS) as raw,
            context.wrap_socket(raw, server_hostname=server.host) as first,
        ):
            http1_get(first, "/")
            session = first.session
        with (
            socket.create_connection(address, timeout=_TIMEOUT_SECONDS) as raw,
            context.wrap_socket(raw, server_hostname=server.host, session=session) as second,
        ):
            http1_get(second, "/")
            assert not second.session_reused


def test_keeps_the_client_hello_of_a_connection_that_stopped_there() -> None:
    raw = samples.client_hello_records([samples.server_name()])
    with CaptureServer() as server:
        with socket.create_connection(("127.0.0.1", server.port(HTTP1)), timeout=_TIMEOUT_SECONDS) as sock:
            sock.sendall(raw)
        connection = wait_for_ended_connection(server, HTTP1)

    assert connection.client_hello.raw == raw
    assert connection.requests == ()
    assert connection.error is not None


def test_survives_a_client_that_does_not_speak_tls(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING), CaptureServer() as server:
        with socket.create_connection(("127.0.0.1", server.port(HTTP1)), timeout=_TIMEOUT_SECONDS) as sock:
            sock.sendall(b"GET / HTTP/1.1\r\n\r\n")
            assert sock.recv(1) == b""
        assert server.recording(HTTP1).connections == ()
        with connect(server.port(HTTP1)) as sock:
            assert http1_get(sock, "/fetch").startswith(b"HTTP/1.1 200")

    assert "before its ClientHello was complete" in caplog.text


def test_records_a_protocol_error_and_closes_the_connection() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP2)) as sock:
            sock.sendall(b"this is no HTTP/2 preface")
        connection = wait_for_ended_connection(server, HTTP2)

    assert connection.error is not None
    assert "preface" in connection.error


def test_refuses_a_port_in_use() -> None:
    with CaptureServer() as first:
        taken = CaptureServer(http1_port=first.port(HTTP1))
        with pytest.raises(OSError) as caught:  # noqa: PT011
            taken.__enter__()

    assert caught.value.errno == errno.EADDRINUSE


def test_command_line_writes_both_recordings(tmp_path: Path) -> None:
    command = [sys.executable, "-m", "tools.capture", "--out", str(tmp_path), "--http1-port", "0", "--http2-port", "0"]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    ) as process:
        assert process.stdout is not None
        ports: dict[str, int] = {}
        printed: list[str] = []
        while len(ports) < 2:
            line = process.stdout.readline()
            printed.append(line)
            match = re.match(r"(HTTP/1\.1|HTTP/2): +https://capture\.localhost:(\d+)/", line)
            if match is not None:
                ports[match.group(1)] = int(match.group(2))
            assert line, "".join(printed)
        for port_name, port in ports.items():
            with connect(port) as sock:
                if port_name == "HTTP/1.1":
                    for kind in RequestKind:
                        http1_get(sock, KIND_PATHS[kind])
                    http1_get(sock, DONE_PATH)
                else:
                    client = Http2Client(sock)
                    for kind in RequestKind:
                        client.get(KIND_PATHS[kind])
                    client.get(DONE_PATH)
        output = process.stdout.read()
        assert process.wait(_TIMEOUT_SECONDS) == 0, output

    for stem, protocol in (("http1", "http/1.1"), ("http2", "h2")):
        form = json.loads((tmp_path / f"{stem}.json").read_text(encoding="utf-8"))
        assert form["protocol"] == protocol
        assert list(form["requests"]) == ["navigation", "fetch", "script", "image"]
        raw_form = json.loads((tmp_path / f"{stem}.raw.json").read_text(encoding="utf-8"))
        assert raw_form["connections"][0]["client_hello"].startswith("1603")


def test_command_line_explains_a_port_in_use(tmp_path: Path) -> None:
    with CaptureServer() as server:
        command = [
            sys.executable,
            "-m",
            "tools.capture",
            "--out",
            str(tmp_path),
            "--http1-port",
            str(server.port(HTTP1)),
        ]
        result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 1
    assert "--http1-port" in result.stderr
    assert "Traceback" not in result.stderr


def test_command_line_refuses_another_host(tmp_path: Path) -> None:
    command = [sys.executable, "-m", "tools.capture", "--out", str(tmp_path), "--host", "example.com"]

    result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 2
    assert "localhost" in result.stderr
