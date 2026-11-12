"""Tests of the capture server through real sockets and real TLS, with Python's own TLS as the client."""

import base64
import contextlib
import errno
import hashlib
import json
import logging
import re
import socket
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import serialization

from tests.capture import samples
from tests.capture.clients import Http2Client, client_context, connect, http1_exchange, http1_get
from tests.capture.http3_client import Http3Client
from tools.capture import (
    DEFAULT_HOST,
    HTTP1,
    HTTP2,
    HTTP3,
    KIND_PATHS,
    PLAIN_HTTP,
    PROXY,
    CaptureServer,
    RequestKind,
    normal_form,
)
from tools.capture.cases import CASE_HOST, HOST_NAMES
from tools.capture.http1 import Http1Request
from tools.capture.http2 import SETTINGS
from tools.capture.page import DONE_PATH
from tools.capture.probe import RecordValue, probe_script
from tools.capture.recording import SCRIPT, ConnectionRecord, QuicConnectionRecord
from tools.capture.script_normal_form import read_script_raw_record
from tools.capture.script_page import PAGE_PATH, RECORD_PATH

_REPOSITORY = Path(__file__).parents[2]
_TIMEOUT_SECONDS = 10.0


def free_ports(count: int, kind: int = socket.SOCK_STREAM) -> list[int]:
    """Return ports that are free now. Holding them all at once keeps them different from each other."""
    sockets = [socket.socket(socket.AF_INET, kind) for _ in range(count)]
    for sock in sockets:
        sock.bind(("127.0.0.1", 0))
    ports = [sock.getsockname()[1] for sock in sockets]
    for sock in sockets:
        sock.close()
    return ports


def free_ports_for_tcp_and_udp(count: int) -> list[int]:
    """Return port numbers that both TCP and UDP can bind now, each different.

    Windows refuses a bind to a port in a range that it excludes, with WSAEACCES, and excludes some ranges for one of the
    two only, such as those that Docker's network reserves. So a number that one of them has free may be refused to the
    other.
    """
    held: list[socket.socket] = []
    ports: list[int] = []
    try:
        for _ in range(count * 100):
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            held.append(tcp)
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                try:
                    udp.bind(("127.0.0.1", port))
                except OSError:
                    continue
            ports.append(port)
            if len(ports) == count:
                return ports
    finally:
        for sock in held:
            sock.close()
    raise AssertionError(f"found only {ports} for both TCP and UDP")


def wait_for_ended_connection(server: CaptureServer, protocol: str) -> ConnectionRecord:
    """Wait until the first connection on a port has ended in an error, which a real client's close does."""
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        connections = server.recording(protocol).connections
        if connections and connections[0].error is not None:
            connection = connections[0]
            assert isinstance(connection, ConnectionRecord)
            return connection
        time.sleep(0.01)
    raise AssertionError("the connection did not end")


def wait_for_condition(condition: Callable[[], bool], *, what: str) -> None:
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while not condition():
        assert time.monotonic() < deadline, what
        time.sleep(0.01)


def only_quic_connection(server: CaptureServer) -> QuicConnectionRecord:
    connections = server.recording(HTTP3).connections
    assert len(connections) == 1
    connection = connections[0]
    assert isinstance(connection, QuicConnectionRecord)
    return connection


def test_a_host_must_be_a_name_nothing_else_has() -> None:
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
        assert names.get_values_for_type(x509.DNSName) == [server.host, *HOST_NAMES]


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


def test_records_http3() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3)) as client:
        assert client.get("/fetch") == (b"200", b"fetched\n")
        assert client.get("/missing") == (b"404", b"not found\n")
        recording = server.recording(HTTP3)

    assert recording.authority == server.authority(HTTP3)
    connection = recording.connections[0]
    assert isinstance(connection, QuicConnectionRecord)
    assert connection.alpn == "h3"
    assert connection.tls_version == "TLSv1.3"
    assert connection.cipher is not None
    assert connection.cipher.startswith("TLS_")
    assert connection.ip_version == 4
    assert connection.peer == "127.0.0.1"
    assert connection.error is None
    assert [request.target for request in connection.requests] == ["/fetch", "/missing"]
    assert connection.requests[0].headers[-1] == ("x-mitto", "test")
    assert connection.client_hello.hello.cipher_suites
    form = normal_form(recording)
    assert form["protocol"] == "h3"
    assert form["requests"] == {
        "fetch": {
            "headers": [
                ":method: GET",
                ":scheme: https",
                ":authority: <authority>",
                ":path: /fetch",
                "x-mitto: test",
            ]
        }
    }
    quic = form["quic"]
    assert isinstance(quic, dict)
    assert quic["version"] == "QUIC v1"


def test_records_http3_over_ipv6() -> None:
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
        try:
            probe.bind(("::1", 0))
        except OSError:
            pytest.skip("no IPv6 loopback here")
    with CaptureServer() as server, Http3Client(server.port(HTTP3), address="::1") as client:
        assert client.get("/fetch") == (b"200", b"fetched\n")
        connection = only_quic_connection(server)
        assert connection.ip_version == 6
        assert connection.peer == "::1"


def test_an_http3_client_gets_the_same_page_as_the_others() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3)) as client:
        status, body = client.get("/")

    assert status == b"200"
    assert b"mitto capture" in body


def test_http3_serves_more_than_one_client() -> None:
    with (
        CaptureServer() as server,
        Http3Client(server.port(HTTP3)) as first,
        Http3Client(server.port(HTTP3)) as second,
    ):
        assert first.get("/fetch")[0] == b"200"
        assert second.get("/image.png")[0] == b"200"
        assert first.get("/script.js")[0] == b"200"
        connections = server.recording(HTTP3).connections

    assert [[request.target for request in connection.requests] for connection in connections] == [
        ["/fetch", "/script.js"],
        ["/image.png"],
    ]


def test_connections_are_numbered_across_ports() -> None:
    with CaptureServer() as server:
        with connect(server.port(HTTP1)) as sock:
            http1_get(sock, "/")
        with Http3Client(server.port(HTTP3)) as client:
            client.get("/")
            connections = server.recording(HTTP3).connections
        with connect(server.port(HTTP2)) as sock:
            Http2Client(sock).get("/")

        assert [connection.id for connection in server.recording(HTTP1).connections] == [1]
        assert [connection.id for connection in connections] == [2]
        assert [connection.id for connection in server.recording(HTTP2).connections] == [3]


def test_http3_needs_h3_through_alpn() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3), alpn=["hq-interop"]) as client:
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while client.terminated is None:
            assert time.monotonic() < deadline
            client.pump()
        connection = only_quic_connection(server)

    assert connection.alpn is None
    assert connection.requests == ()


def test_quic_keeps_the_client_hello_of_a_connection_that_stopped_there() -> None:
    datagrams = samples.initial_datagrams(samples.client_hello_message([samples.server_name()]))
    with CaptureServer() as server, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for datagram in datagrams:
            sock.sendto(datagram, ("127.0.0.1", server.port(HTTP3)))
        wait_for_condition(lambda: bool(server.recording(HTTP3).connections), what="no connection recorded")
        connection = only_quic_connection(server)

    assert connection.client_hello.datagrams == tuple(datagrams)
    assert connection.requests == ()


def test_survives_datagrams_that_are_not_quic(caplog: pytest.LogCaptureFixture) -> None:
    undecryptable = samples.initial_packet(bytes(1200), keys_from=b"other keys")
    with (
        caplog.at_level(logging.WARNING),
        CaptureServer() as server,
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock,
    ):
        address = ("127.0.0.1", server.port(HTTP3))
        for garbage in (b"", b"\x00", b"this is no QUIC packet", b"\xc0" + bytes(32), undecryptable):
            sock.sendto(garbage, address)
        with Http3Client(server.port(HTTP3)) as client:
            assert client.get("/fetch")[0] == b"200"
        assert len(server.recording(HTTP3).connections) == 1

    assert "opened no connection" in caplog.text


def test_tells_a_client_of_another_version_what_it_speaks() -> None:
    with (
        CaptureServer() as server,
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock,
    ):
        sock.settimeout(_TIMEOUT_SECONDS)
        probe = b"\xc0" + (0x1A2A3A4A).to_bytes(4) + b"\x01a\x01b" + bytes(1200)
        sock.sendto(probe, ("127.0.0.1", server.port(HTTP3)))
        reply = sock.recv(1 << 16)

    assert reply[1:5] == bytes(4)
    assert reply[-4:] == (1).to_bytes(4)


def test_records_an_http3_protocol_error_and_closes_the_connection() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3)) as client:
        assert client.get("/fetch")[0] == b"200"
        stream_id = client.quic.get_next_available_stream_id()
        client.quic.send_stream_data(stream_id, b"\x01\x03\xff\xff\xff", end_stream=True)
        client.flush()
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while client.terminated is None:
            assert time.monotonic() < deadline
            client.pump()
        connection = only_quic_connection(server)

    assert connection.error is not None
    assert "header section" in connection.error
    assert client.terminated.error_code == 0x101


def test_records_how_a_quic_client_left() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3)) as client:
        assert client.get("/fetch")[0] == b"200"
        client.quic.close(error_code=0x123, reason_phrase="tired")
        client.flush()
        wait_for_condition(lambda: only_quic_connection(server).error is not None, what="the connection did not end")
        connection = only_quic_connection(server)

    assert connection.error is not None
    assert "0x123" in connection.error


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


def test_the_test_page_ends_with_the_done_path_over_http3() -> None:
    with CaptureServer() as server, Http3Client(server.port(HTTP3)) as client:
        for kind in RequestKind:
            client.get(KIND_PATHS[kind])
        assert not server.wait_for_page(HTTP3, 0)
        client.get(DONE_PATH)
        assert server.wait_for_page(HTTP3, _TIMEOUT_SECONDS)
        assert not server.wait_for_page(HTTP2, 0)

        requests = normal_form(server.recording(HTTP3))["requests"]
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

    assert connection.client_hello is not None
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
            # Closing with data unread resets the connection on Windows, which can reach the server before the preface.
            with contextlib.suppress(OSError):
                while sock.recv(1 << 16):
                    pass
        connection = wait_for_ended_connection(server, HTTP2)

    assert connection.error is not None
    assert "preface" in connection.error


def test_starts_only_once() -> None:
    with CaptureServer() as server, pytest.raises(RuntimeError, match="started before"):
        server.start()


@pytest.mark.parametrize("protocol", [HTTP1, HTTP3])
def test_refuses_a_port_in_use(protocol: str) -> None:
    with CaptureServer() as first:
        port = first.port(protocol)
        taken = CaptureServer(http1_port=port) if protocol == HTTP1 else CaptureServer(http3_port=port)
        with pytest.raises(OSError) as caught:  # noqa: PT011
            taken.__enter__()

    assert caught.value.errno == errno.EADDRINUSE


def test_a_named_http3_port_serves_there() -> None:
    port = free_ports(1, socket.SOCK_DGRAM)[0]
    with CaptureServer(http3_port=port) as server, Http3Client(port) as client:
        assert server.port(HTTP3) == port
        assert server.url(HTTP3) == f"https://{DEFAULT_HOST}:{port}/"
        assert client.get("/fetch")[0] == b"200"


def steer_first_bind(
    monkeypatch: pytest.MonkeyPatch, kind: socket.SocketKind, number: Callable[[list[int]], int]
) -> None:
    """Send the first bind to port 0 of a socket of kind to a number, as the kernel may by chance.

    Args:
        monkeypatch: Undoes this when the test ends.
        kind: The kind of socket to steer.
        number: Returns the number, given the TCP ports bound so far.
    """
    real_bind = socket.socket.bind
    tcp_ports: list[int] = []
    pending = [number]

    def bind(sock: socket.socket, address: tuple[str, int]) -> None:
        host, port = address
        if sock.type == kind and port == 0 and pending:
            port = pending.pop()(tcp_ports)
        real_bind(sock, (host, port))
        if sock.type == socket.SOCK_STREAM:
            tcp_ports.append(sock.getsockname()[1])

    monkeypatch.setattr(socket.socket, "bind", bind)


def steer_tcp_binds(monkeypatch: pytest.MonkeyPatch, ports: list[int]) -> None:
    """Send each TCP bind to port 0 to the next of ports, while any are left.

    Args:
        monkeypatch: Undoes this when the test ends.
        ports: The numbers, in the order the binds get them.
    """
    real_bind = socket.socket.bind
    left = list(ports)

    def bind(sock: socket.socket, address: tuple[str, int]) -> None:
        host, port = address
        if sock.type == socket.SOCK_STREAM and port == 0 and left:
            port = left.pop(0)
        real_bind(sock, (host, port))

    monkeypatch.setattr(socket.socket, "bind", bind)


@pytest.mark.parametrize(("route", "position"), [(HTTP1, 0), (HTTP2, 1), (PLAIN_HTTP, 2), (PROXY, 3)])
def test_a_chosen_http3_port_does_not_take_the_number_of_a_tcp_port(
    monkeypatch: pytest.MonkeyPatch, route: str, position: int
) -> None:
    """TCP to the HTTP/3 port must be refused, not reach another port's listener and get a response."""
    # Numbers that UDP can bind too, so that the steered bind below meets the server's check rather than a refusal.
    steer_tcp_binds(monkeypatch, free_ports_for_tcp_and_udp(8))
    # Each of the four TCP ports listens on one or two loopback addresses, whichever the machine has.
    steer_first_bind(monkeypatch, socket.SOCK_DGRAM, lambda tcp_ports: tcp_ports[position * len(tcp_ports) // 4])

    with CaptureServer() as server:
        assert server.port(HTTP3) != server.port(route)


def test_a_chosen_tcp_port_does_not_take_the_number_of_a_named_http3_port(monkeypatch: pytest.MonkeyPatch) -> None:
    named = free_ports_for_tcp_and_udp(1)[0]
    steer_first_bind(monkeypatch, socket.SOCK_STREAM, lambda _: named)

    with CaptureServer(http3_port=named) as server:
        assert server.port(HTTP3) == named
        assert named not in (server.port(HTTP1), server.port(HTTP2))


def test_named_ports_may_share_a_number_across_tcp_and_udp() -> None:
    port = free_ports_for_tcp_and_udp(1)[0]

    with CaptureServer(http1_port=port, http3_port=port) as server:
        assert server.port(HTTP1) == server.port(HTTP3) == port


def test_command_line_writes_the_recordings_of_its_runs(tmp_path: Path) -> None:
    # Named ports, as by default: a server that starts twice gets away with it on port 0.
    http1_port, http2_port, plain_http_port, proxy_port, script_port = free_ports(5)
    http3_port = free_ports(1, socket.SOCK_DGRAM)[0]
    command = [
        sys.executable,
        "-m",
        "tools.capture",
        "--out",
        str(tmp_path),
        "--script-port",
        str(script_port),
        "--http1-port",
        str(http1_port),
        "--http2-port",
        str(http2_port),
        "--http3-port",
        str(http3_port),
        "--plain-http-port",
        str(plain_http_port),
        "--proxy-port",
        str(proxy_port),
        "--runs",
        "http1",
        "http2",
        "http3",
    ]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    ) as process:
        assert process.stdout is not None
        ports: dict[str, int] = {}
        printed: list[str] = []
        while len(ports) < 3:
            line = process.stdout.readline()
            printed.append(line)
            match = re.match(r"(HTTP/1\.1|HTTP/2|HTTP/3): +https://capture\.localhost:(\d+)/", line)
            if match is not None:
                ports[match.group(1)] = int(match.group(2))
            assert line, "".join(printed)
        assert ports == {"HTTP/1.1": http1_port, "HTTP/2": http2_port, "HTTP/3": http3_port}
        while "--origin-to-force-quic-on" not in "".join(printed):
            printed.append(process.stdout.readline())
            assert printed[-1], "".join(printed)
        assert f"--origin-to-force-quic-on=capture.localhost:{http3_port}" in "".join(printed)
        with Http3Client(http3_port) as http3_client:
            for kind in RequestKind:
                http3_client.get(KIND_PATHS[kind])
            http3_client.get(DONE_PATH)
        for port_name, port in ports.items():
            if port_name == "HTTP/3":
                continue
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

    for stem, protocol, client_hello_start in (
        ("http1", "http/1.1", "1603"),
        ("http2", "h2", "1603"),
        ("http3", "h3", "01"),
    ):
        form = json.loads((tmp_path / f"{stem}.json").read_text(encoding="utf-8"))
        assert form["protocol"] == protocol
        assert list(form["requests"]) == ["navigation", "fetch", "script", "image"]
        raw_form = json.loads((tmp_path / f"{stem}.raw.json").read_text(encoding="utf-8"))
        assert raw_form["connections"][0]["client_hello"].startswith(client_hello_start)
        assert raw_form["connections"][0]["peer"] == "127.0.0.1"


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
            "--http2-port",
            str(free_ports(1)[0]),
        ]
        result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 1
    assert "--http1-port" in result.stderr
    assert "Traceback" not in result.stderr


def test_command_line_explains_a_udp_port_in_use(tmp_path: Path) -> None:
    with CaptureServer() as server:
        command = [
            sys.executable,
            "-m",
            "tools.capture",
            "--out",
            str(tmp_path),
            "--http3-port",
            str(server.port(HTTP3)),
            "--http1-port",
            str(free_ports(1)[0]),
            "--http2-port",
            str(free_ports(1)[0]),
        ]
        result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 1
    assert "--http3-port" in result.stderr
    assert "Traceback" not in result.stderr


def test_command_line_refuses_another_host(tmp_path: Path) -> None:
    command = [sys.executable, "-m", "tools.capture", "--out", str(tmp_path), "--host", "example.com"]

    result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 2
    assert "localhost" in result.stderr


def test_the_page_of_a_script_fingerprint_runs_the_probe_and_takes_its_record() -> None:
    record = b'{"format": 1, "objects": [], "values": {}, "reports": []}'
    with CaptureServer() as server:
        authority = server.authority(SCRIPT, host=CASE_HOST)
        with connect(server.port(SCRIPT), host=CASE_HOST) as sock:
            assert sock.selected_alpn_protocol() == "http/1.1"
            page = http1_exchange(sock, "GET", PAGE_PATH, authority=authority)
            posted = http1_exchange(sock, "POST", RECORD_PATH, authority=authority, body=record)
            other = http1_exchange(sock, "GET", KIND_PATHS[RequestKind.NAVIGATION], authority=authority)
        assert server.wait_for_page(SCRIPT, _TIMEOUT_SECONDS, done_path=RECORD_PATH)
        recording = server.recording(SCRIPT, host=CASE_HOST)

    text = page.body.decode()
    assert (page.status, page.headers["cache-control"]) == (200, "no-store")
    assert json.dumps(probe_script()).replace("<", "\\u003c") in text
    assert text.count("</script>") == 1
    # Nothing that would be a property of the window, which the probe reads.
    assert " id=" not in text
    assert " name=" not in text
    assert (posted.status, other.status) == (200, 404)
    (connection,) = recording.connections
    assert isinstance(connection, ConnectionRecord)
    request = connection.requests[1]
    assert isinstance(request, Http1Request)
    assert request.body == record


def test_the_wires_routes_do_not_record_the_page_of_a_script_fingerprint() -> None:
    with CaptureServer() as server:
        with connect(server.port(SCRIPT), host=CASE_HOST) as sock:
            http1_get(sock, PAGE_PATH)

        assert all(server.recording(route).connections == () for route in (HTTP1, HTTP2, HTTP3, PLAIN_HTTP))


def test_command_line_writes_the_record_of_a_script_fingerprint_and_its_normal_form(tmp_path: Path) -> None:
    ports = free_ports(5)
    http3_port = free_ports(1, socket.SOCK_DGRAM)[0]
    record: dict[str, RecordValue] = {
        "format": 1,
        "objects": [{"path": "globalThis", "kind": "instance", "get": [["innerWidth", 1371], ["origin", "x"]]}],
        "values": {},
        "reports": [],
    }
    command = [
        sys.executable,
        "-m",
        "tools.capture",
        "--out",
        str(tmp_path),
        *("--http1-port", str(ports[0]), "--http2-port", str(ports[1]), "--http3-port", str(http3_port)),
        *("--plain-http-port", str(ports[2]), "--proxy-port", str(ports[3]), "--script-port", str(ports[4])),
        "--runs",
        "script/page",
    ]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    ) as process:
        assert process.stdout is not None
        printed: list[str] = []
        while not printed or not printed[-1].startswith("Waiting"):
            printed.append(process.stdout.readline())
            assert printed[-1], "".join(printed)
        assert f"Script Fingerprint of a top-level page: https://capture.test:{ports[4]}{PAGE_PATH}\n" in printed
        with connect(ports[4], host=CASE_HOST) as sock:
            http1_exchange(
                sock, "POST", RECORD_PATH, authority=f"{CASE_HOST}:{ports[4]}", body=json.dumps(record).encode()
            )
        output = process.stdout.read()
        assert process.wait(_TIMEOUT_SECONDS) == 0, output

    (run,) = json.loads((tmp_path / "runs.json").read_text(encoding="utf-8"))["runs"]
    assert (run["name"], run["fingerprint"]) == ("script/page", "script")
    raw = read_script_raw_record((tmp_path / "script" / "page.raw.json").read_text(encoding="utf-8"))
    assert (raw.url, raw.peers, raw.record) == (f"https://capture.test:{ports[4]}{PAGE_PATH}", ("127.0.0.1",), record)
    form = (tmp_path / "script" / "page.json").read_text(encoding="utf-8")
    assert '  ["innerWidth", "<machine>"],' in form.splitlines()


def test_command_line_explains_a_page_that_posted_no_record(tmp_path: Path) -> None:
    ports = free_ports(5)
    http3_port = free_ports(1, socket.SOCK_DGRAM)[0]
    command = [
        sys.executable,
        "-m",
        "tools.capture",
        "--out",
        str(tmp_path),
        *("--http1-port", str(ports[0]), "--http2-port", str(ports[1]), "--http3-port", str(http3_port)),
        *("--plain-http-port", str(ports[2]), "--proxy-port", str(ports[3]), "--script-port", str(ports[4])),
        "--runs",
        "script/page",
    ]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    ) as process:
        assert process.stdout is not None
        while not process.stdout.readline().startswith("Waiting"):
            pass
        with connect(ports[4], host=CASE_HOST) as sock:
            http1_exchange(sock, "POST", RECORD_PATH, authority=f"{CASE_HOST}:{ports[4]}", body=b"[]")
        _, errors = process.communicate(timeout=_TIMEOUT_SECONDS)

    assert process.returncode == 1
    assert "the page posted no record of the probe" in errors
    assert not (tmp_path / "script" / "page.json").exists()
