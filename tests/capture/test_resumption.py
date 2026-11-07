"""Tests of the capture server when it gives out session tickets, with Python's own TLS and aioquic as the clients."""

import contextlib
import json
import socket
import ssl
import subprocess
import sys
import time
from collections.abc import Iterable
from pathlib import Path
from typing import IO

import pytest

from tests.capture import samples
from tests.capture.clients import Http2Client, client_context, http1_get
from tests.capture.http3_client import Http3Client, session_ticket_after
from tools.capture import (
    CASE_HOST,
    CASES,
    CROSS_SITE_HOST,
    HTTP1,
    HTTP2,
    HTTP3,
    KIND_PATHS,
    RESUMPTION_CLOSE_PATH,
    RESUMPTION_RUNS,
    RUNS,
    CaptureServer,
    Recording,
    RequestKind,
    Run,
    normal_form,
    raw_form,
)
from tools.capture.cases import respond_on
from tools.capture.http2 import DATA, GOAWAY, HEADERS
from tools.capture.names import extension_name
from tools.capture.recording import ConnectionRecord, Json, QuicConnectionRecord

_REPOSITORY = Path(__file__).parents[2]
_TIMEOUT_SECONDS = 10.0
_PSK = "pre_shared_key"
_FETCH = KIND_PATHS[RequestKind.FETCH]
_FETCH_CASE = next(case for case in CASES if case.path == _FETCH)
_RUNS_BY_ROUTE = {run.route: run for run in RESUMPTION_RUNS}
# A ticket only goes with the context that got it.
_CONTEXTS = {route: client_context((route,)) for route in (HTTP1, HTTP2)}


class _Head:
    headers: tuple[tuple[str, str], ...] = ()

    def __init__(self, target: str) -> None:
        self.target = target


def ask(sock: ssl.SSLSocket, route: str, path: str) -> None:
    """Make a request, and read what the server says after the handshake, which holds its tickets."""
    if route == HTTP1:
        http1_get(sock, path)
        return
    Http2Client(sock).get(path)
    sock.settimeout(0.2)
    with contextlib.suppress(TimeoutError):
        sock.recv(1 << 16)
    sock.settimeout(_TIMEOUT_SECONDS)


def go_over_tcp(
    server_port: int, route: str, host: str, paths: Iterable[str], session: ssl.SSLSession | None = None
) -> ssl.SSLSession | None:
    """Ask for each path from a new connection, each with the ticket of the one before. Returns the last one's ticket."""
    for path in paths:
        with (
            socket.create_connection(("127.0.0.1", server_port), timeout=_TIMEOUT_SECONDS) as raw,
            _CONTEXTS[route].wrap_socket(raw, server_hostname=host, session=session) as sock,
        ):
            ask(sock, route, path)
            session = sock.session
    return session


def go_over_http3(server: CaptureServer, *, early: bool = True) -> Recording:
    """Make a request and then, from a new connection with its ticket, another. Returns what the server recorded."""
    with Http3Client(server.port(HTTP3), keep_tickets=True) as first:
        first.get(_FETCH)
        ticket = session_ticket_after(first)
    with Http3Client(server.port(HTTP3), session_ticket=ticket) as second:
        if not early:
            second.wait_for_handshake()
        second.get(_FETCH)
        # The answer can come before the server has read the end of the handshake, which comes before the end of this.
        second.wait_for_handshake()
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while not server.recording(HTTP3).connections[1].resumed:
            assert time.monotonic() < deadline, "the server did not resume"
            second.pump()
    return server.recording(HTTP3)


def extension_names(form: dict[str, Json]) -> set[str]:
    tls = form["tls"]
    assert isinstance(tls, dict)
    extensions = tls["extensions"]
    assert isinstance(extensions, list)
    return {str(extension["type"]) for extension in extensions if isinstance(extension, dict)}


def request_names(form: dict[str, Json]) -> list[str]:
    requests = form["requests"]
    assert isinstance(requests, dict)
    return list(requests)


def request_of(form: dict[str, Json]) -> dict[str, Json]:
    requests = form["requests"]
    assert isinstance(requests, dict)
    request = next(iter(requests.values()))
    assert isinstance(request, dict)
    return request


def test_a_case_of_a_resumption_run_is_none_of_the_others() -> None:
    names = {case.name for case in CASES}
    paths = {case.path for case in CASES}

    for run in RESUMPTION_RUNS:
        assert run.name not in {other.name for other in RUNS}
        for case in (*run.cases, *run.fresh_cases):
            assert case.name not in names
            assert case.path not in paths


def test_no_answer_of_the_other_cases_ends_its_connection() -> None:
    assert respond_on("https", 1, _Head(RESUMPTION_CLOSE_PATH)).close
    assert not any(respond_on("https", 1, _Head(case.path)).close for case in CASES)


@pytest.mark.parametrize("route", [HTTP1, HTTP2])
def test_a_second_connection_over_tcp_resumes_with_a_ticket(route: str) -> None:
    with CaptureServer(tickets=True) as server:
        go_over_tcp(server.port(route), route, server.host, [_FETCH, _FETCH])
        recording = server.recording(route)

    fresh, resumed = recording.connections
    assert isinstance(fresh, ConnectionRecord)
    assert isinstance(resumed, ConnectionRecord)
    assert fresh.client_hello is not None
    assert resumed.client_hello is not None
    assert not fresh.resumed
    assert resumed.resumed
    assert _PSK not in {extension_name(extension.type) for extension in fresh.client_hello.hello.extensions}
    assert _PSK in {extension_name(extension.type) for extension in resumed.client_hello.hello.extensions}


def test_the_raw_recording_says_resumed_only_of_a_connection_that_is() -> None:
    with CaptureServer(tickets=True) as server:
        go_over_tcp(server.port(HTTP1), HTTP1, server.host, [_FETCH, _FETCH])
        raw = raw_form(server.recording(HTTP1))

    assert isinstance(raw, dict)
    connections = raw["connections"]
    assert isinstance(connections, list)
    assert ["resumed" in connection for connection in connections if isinstance(connection, dict)] == [False, True]


def test_without_tickets_a_second_connection_stays_fresh_and_its_raw_recording_as_it_was() -> None:
    with CaptureServer() as server:
        go_over_tcp(server.port(HTTP1), HTTP1, server.host, [_FETCH, _FETCH])
        recording = server.recording(HTTP1)

    assert [connection.resumed for connection in recording.connections] == [False, False]
    assert '"resumed"' not in json.dumps(raw_form(recording))


def test_an_answer_that_ends_its_connection_over_http1_says_so() -> None:
    with (
        CaptureServer(tickets=True) as server,
        socket.create_connection(("127.0.0.1", server.port(HTTP1)), timeout=_TIMEOUT_SECONDS) as raw,
        client_context((HTTP1,)).wrap_socket(raw, server_hostname=server.host) as sock,
    ):
        assert b"Connection: close\r\n" in http1_get(sock, RESUMPTION_CLOSE_PATH)
        assert sock.recv(1) == b""


def test_an_answer_that_ends_its_connection_over_http2_is_followed_by_goaway() -> None:
    with (
        CaptureServer(tickets=True) as server,
        socket.create_connection(("127.0.0.1", server.port(HTTP2)), timeout=_TIMEOUT_SECONDS) as raw,
        client_context((HTTP2,)).wrap_socket(raw, server_hostname=server.host) as sock,
    ):
        client = Http2Client(sock)
        sock.sendall(samples.http2_request_frame(RESUMPTION_CLOSE_PATH, encoder=client.encoder))
        frames = client.read_frames_until(GOAWAY)

    assert [frame.type for frame in frames if frame.stream_id == 1] == [HEADERS, DATA]
    assert int.from_bytes(frames[-1].payload[:4]) == 1
    assert int.from_bytes(frames[-1].payload[4:8]) == 0


def test_an_answer_that_ends_its_connection_over_http3_is_followed_by_goaway() -> None:
    with CaptureServer(tickets=True) as server, Http3Client(server.port(HTTP3)) as client:
        status, _ = client.get(RESUMPTION_CLOSE_PATH)

        assert status == b"200"
        assert client.wait_for_goaway() == 4


def test_a_second_connection_over_http3_resumes_and_sends_a_request_early() -> None:
    with CaptureServer(tickets=True) as server:
        recording = go_over_http3(server)

    fresh, resumed = recording.connections
    assert isinstance(fresh, QuicConnectionRecord)
    assert isinstance(resumed, QuicConnectionRecord)
    assert not fresh.resumed
    assert fresh.early_streams == frozenset()
    assert resumed.resumed
    assert 0 in resumed.early_streams


def test_the_normal_form_tells_a_request_sent_early_from_one_that_waited() -> None:
    def early_data_of_the_second(*, early: bool) -> list[Json]:
        with CaptureServer(tickets=True) as server:
            recording = go_over_http3(server, early=early)
        second = Recording(recording.host, recording.authority, recording.connections[1:])
        early_data = request_of(normal_form(second, [_FETCH_CASE])).get("early_data", [])
        assert isinstance(early_data, list)
        return early_data

    assert "request" in early_data_of_the_second(early=True)
    assert "request" not in early_data_of_the_second(early=False)


def test_the_normal_form_of_a_fresh_connection_says_nothing_of_early_data() -> None:
    with CaptureServer(tickets=True) as server, Http3Client(server.port(HTTP3)) as client:
        client.get(_FETCH)
        form = normal_form(server.recording(HTTP3), [_FETCH_CASE])

    assert "early_data" not in request_of(form)


def test_the_server_reads_datagrams_late_only_with_tickets() -> None:
    def fastest_get_in_seconds(*, tickets: bool) -> float:
        with CaptureServer(tickets=tickets) as server, Http3Client(server.port(HTTP3)) as client:
            client.wait_for_handshake()
            times = []
            for _ in range(5):
                start = time.monotonic()
                client.get(_FETCH)
                times.append(time.monotonic() - start)
            return min(times)

    assert fastest_get_in_seconds(tickets=True) >= 0.1
    assert fastest_get_in_seconds(tickets=False) < 0.1


@pytest.mark.parametrize("route", [HTTP1, HTTP2])
def test_a_client_can_produce_every_case_of_a_resumption_run(route: str) -> None:
    run = _RUNS_BY_ROUTE[route]
    with CaptureServer(tickets=True) as server:
        port = server.port(route)
        go_over_tcp(port, route, CROSS_SITE_HOST, [case.path for case in run.fresh_cases])
        session = go_over_tcp(port, route, CASE_HOST, [run.start_path])
        go_over_tcp(port, route, CASE_HOST, [case.path for case in run.cases], session)
        recording = server.recording(route, host=CASE_HOST)

    fresh = normal_form(recording, run.fresh_cases)
    resumed = normal_form(recording, run.cases)
    assert _PSK not in extension_names(fresh)
    assert _PSK in extension_names(resumed)
    assert request_names(fresh) == [case.name for case in run.fresh_cases]
    assert request_names(resumed) == [case.name for case in run.cases]


def _printed_port(stream: IO[str], run: Run) -> int:
    for line in stream:
        if line.startswith(run.label):
            return int(line.rsplit(":", 1)[1].split("/", 1)[0])
    raise AssertionError("the server printed no address")


def test_command_line_with_tickets_writes_a_file_for_each_kind_of_connection(tmp_path: Path) -> None:
    run = _RUNS_BY_ROUTE[HTTP1]
    command = [sys.executable, "-m", "tools.capture", "--out", str(tmp_path), "--tickets", "--runs", run.name]
    for flag in ("--http1-port", "--http2-port", "--http3-port", "--plain-http-port", "--proxy-port"):
        command += [flag, "0"]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    ) as process:
        assert process.stdout is not None
        try:
            port = _printed_port(process.stdout, run)
            session = go_over_tcp(port, HTTP1, CASE_HOST, [run.start_path])
            go_over_tcp(port, HTTP1, CASE_HOST, [case.path for case in run.cases], session)
            go_over_tcp(port, HTTP1, CROSS_SITE_HOST, [*(case.path for case in run.fresh_cases), run.done_path])
            output = process.stdout.read()
            assert process.wait(_TIMEOUT_SECONDS) == 0, output
        finally:
            process.kill()

    resumed = json.loads((tmp_path / f"{run.name}.json").read_text(encoding="utf-8"))
    fresh = json.loads((tmp_path / f"{run.name}.fresh.json").read_text(encoding="utf-8"))
    assert _PSK in extension_names(resumed)
    assert _PSK not in extension_names(fresh)
    assert (tmp_path / f"{run.name}.raw.json").exists()


@pytest.mark.parametrize(
    "arguments", [["--runs", RESUMPTION_RUNS[0].name], ["--tickets", "--runs", RUNS[0].name]], ids=["without", "with"]
)
def test_command_line_refuses_a_run_that_does_not_go_with_how_the_server_runs(
    tmp_path: Path, arguments: list[str]
) -> None:
    command = [sys.executable, "-m", "tools.capture", "--out", str(tmp_path), *arguments]

    result = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert result.returncode == 2
    assert "--tickets" in result.stderr
