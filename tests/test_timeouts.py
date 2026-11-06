"""Tests for a request's timers.

Tests that take the native_library fixture go through the real native core to local servers, so they need no network
connection, and are skipped when this machine has no native library. A server here never answers, or answers too slowly.
Each test that expects one timer to run out also sets a long total time, so that a timer that does not run cannot hang
the tests.
"""

import ctypes
import gzip
import http.server
import math
import socket
import threading
import time
from collections.abc import Generator, Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, urlsplit

import pytest

import mitto
from mitto import Timeout, _native
from mitto._native import BodyAvailable, ResponseStarted
from mitto._persona import ENGINE_RELEASE_PERSONA, load_persona
from mitto._request import build_request
from tests.local_server import LocalHTTPServer
from tools.capture import HTTP2, HTTP3, CaptureServer
from tools.parity.replay import run_of, session_settings

ERR_CONNECTION_TIMED_OUT = -118
ERR_INVALID_ARGUMENT = -4
ERR_TIMED_OUT = -7

# What a timer under test allows.
SHORT_SECONDS = 0.2
# What a timer allows that a local server must stay within, even on a busy machine.
ROOMY_SECONDS = 1.0
# A read timer that finds that bytes have come since it started takes one more turn, so this outlasts two.
LONGER_THAN_A_STALLED_READ_SECONDS = ROOMY_SECONDS * 2.5
# What a timer allows that must not run out, and the time that shows that it did not.
LONG_SECONDS = 30.0
WELL_BEFORE_LONG_SECONDS = 15.0

# What a server waits before each part of a trickle, which takes longer than ROOMY_SECONDS in all.
PAUSE_SECONDS = 0.1
TRICKLE_CHUNKS = 15
BODY = b"the whole body"

# A body larger than the socket buffers of a loopback connection hold, and how a slow server reads it.
LARGE_BODY_SIZE = 32 * 1024 * 1024
READ_SIZE_OF_A_SLOW_SERVER = 1024 * 1024
READ_A_HEAD_SIZE = 4096

MAX_TIMEOUT_SECONDS = 2_000_000
MAX_TIMEOUT_MS = 2**31 - 1


class SlowServer(LocalHTTPServer):
    """An HTTP/1.1 server whose paths answer slowly, or stop halfway."""

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, SlowHandler)
        self.stop = threading.Event()

    def url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.server_port}{path}"


class SlowHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast(SlowServer, self.server)
        url = urlsplit(self.path)
        match url.path:
            case "/ok":
                self.send_body_headers(len(BODY))
                self.wfile.write(BODY)
            case "/delayed":
                time.sleep(SHORT_SECONDS * 3)
                self.send_body_headers(len(BODY))
                self.wfile.write(BODY)
            case "/stops-halfway":
                self.send_body_headers(len(BODY) + 1)
                self.wfile.write(BODY)
                self.wfile.flush()
                server.stop.wait(timeout=LONG_SECONDS)
            case "/trickle":
                self.trickle(BODY * TRICKLE_CHUNKS, chunk_size=len(BODY))
            case "/gzip-trickle":
                self.trickle(gzip.compress(BODY), chunk_size=1, content_encoding="gzip")
            case "/endless-trickle":
                self.send_body_headers(1 << 30)
                while not server.stop.wait(timeout=PAUSE_SECONDS):
                    self.wfile.write(BODY)
                    self.wfile.flush()
            case "/redirect":
                self.send_response(302)
                self.send_header("Location", parse_qs(url.query)["to"][0])
                self.send_header("Content-Length", "0")
                self.end_headers()
            case _:
                raise AssertionError(self.path)

    def send_body_headers(self, size: int, content_encoding: str | None = None) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        if content_encoding is not None:
            self.send_header("Content-Encoding", content_encoding)
        self.end_headers()
        self.wfile.flush()

    def trickle(self, body: bytes, chunk_size: int, content_encoding: str | None = None) -> None:
        self.send_body_headers(len(body), content_encoding)
        for start in range(0, len(body), chunk_size):
            time.sleep(PAUSE_SECONDS)
            self.wfile.write(body[start : start + chunk_size])
            self.wfile.flush()

    def handle(self) -> None:
        # A request that a timer ends goes away in the middle of what this sends.
        try:
            super().handle()
        except ConnectionError:
            self.close_connection = True

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def slow_server() -> Iterator[SlowServer]:
    """Run the local server with the paths above."""
    with SlowServer(("127.0.0.1", 0)) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield server
        server.stop.set()
        server.shutdown()


@pytest.fixture
def silent_port() -> Iterator[int]:
    """Return the port of a server that never answers: the system accepts each connection, and nothing reads from it.

    Over http:// a request gets its connection and then waits for the headers. Over https:// it waits for the TLS handshake,
    so it never gets a connection that it can use.
    """
    with socket.create_server(("127.0.0.1", 0)) as listener:
        yield listener.getsockname()[1]


@contextmanager
def takes_at_least(seconds: float) -> Generator[None]:
    """Check that the block takes that long at least, and far less than LONG_SECONDS."""
    start = time.monotonic()
    yield
    elapsed = time.monotonic() - start
    assert seconds <= elapsed < WELL_BEFORE_LONG_SECONDS


@pytest.mark.usefixtures("native_library")
def test_a_number_means_the_total_time(silent_port: int) -> None:
    with (
        mitto.Session() as session,
        takes_at_least(SHORT_SECONDS),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(f"http://127.0.0.1:{silent_port}/", timeout=SHORT_SECONDS)

    assert caught.value.net_error == ERR_TIMED_OUT
    assert caught.value.name == "ERR_TIMED_OUT"


@pytest.mark.usefixtures("native_library")
def test_each_timer_leaves_the_session_working(silent_port: int, slow_server: SlowServer) -> None:
    timers = [
        (f"https://127.0.0.1:{silent_port}/", Timeout(connect=SHORT_SECONDS, total=LONG_SECONDS)),
        (f"http://127.0.0.1:{silent_port}/", Timeout(read=SHORT_SECONDS, total=LONG_SECONDS)),
        (f"http://127.0.0.1:{silent_port}/", Timeout(total=SHORT_SECONDS)),
    ]

    with mitto.Session() as session:
        for url, timeout in timers:
            with pytest.raises(mitto.RequestTimeoutError):
                session.get(url, timeout=timeout)
            assert session.get(slow_server.url("/ok")).content == BODY


@pytest.mark.usefixtures("native_library")
def test_the_timeout_error_is_the_builtin_one(silent_port: int) -> None:
    with mitto.Session() as session, pytest.raises(TimeoutError):
        session.get(f"http://127.0.0.1:{silent_port}/", timeout=SHORT_SECONDS)


@pytest.mark.usefixtures("native_library")
def test_the_connect_timer_runs_through_the_tls_handshake(silent_port: int) -> None:
    url = f"https://127.0.0.1:{silent_port}/"

    with (
        mitto.Session() as session,
        takes_at_least(SHORT_SECONDS),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(url, timeout=Timeout(connect=SHORT_SECONDS, total=LONG_SECONDS))

    assert caught.value.url == url
    assert caught.value.net_error == ERR_CONNECTION_TIMED_OUT
    assert caught.value.name == "ERR_CONNECTION_TIMED_OUT"


@pytest.mark.usefixtures("native_library")
def test_the_connect_timer_runs_through_the_quic_handshake() -> None:
    # This socket takes the first packet of the handshake, and nothing reads it.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
        silent.bind(("127.0.0.1", 0))
        authority = f"127.0.0.1:{silent.getsockname()[1]}"
        with (
            mitto.Session(_settings={"origins_to_force_quic_on": [authority]}) as session,
            pytest.raises(mitto.RequestTimeoutError) as caught,
        ):
            session.get(f"https://{authority}/", timeout=Timeout(connect=SHORT_SECONDS, total=LONG_SECONDS))

    assert caught.value.net_error == ERR_CONNECTION_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_connect_timer_stops_at_the_connection(silent_port: int) -> None:
    # Only the total time can end this, so it ends with that timer's error, long after the connect timer would have run out.
    with (
        mitto.Session() as session,
        takes_at_least(ROOMY_SECONDS * 2),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(f"http://127.0.0.1:{silent_port}/", timeout=Timeout(connect=ROOMY_SECONDS, total=ROOMY_SECONDS * 2))

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_connect_timer_starts_over_at_each_redirect(silent_port: int, slow_server: SlowServer) -> None:
    url = slow_server.url(f"/redirect?to=https://127.0.0.1:{silent_port}/")

    with mitto.Session() as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.get(url, timeout=Timeout(connect=SHORT_SECONDS, total=LONG_SECONDS))

    assert caught.value.url == url
    assert caught.value.net_error == ERR_CONNECTION_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_read_timer_runs_while_waiting_for_the_headers(silent_port: int) -> None:
    with (
        mitto.Session() as session,
        takes_at_least(SHORT_SECONDS),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(f"http://127.0.0.1:{silent_port}/", timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS))

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_read_timer_waits_for_the_connection_to_start(silent_port: int) -> None:
    # The connect time is not the read timer's to count, so this outlasts it.
    with mitto.Session() as session, takes_at_least(SHORT_SECONDS * 3), pytest.raises(mitto.RequestTimeoutError):
        session.get(f"https://127.0.0.1:{silent_port}/", timeout=Timeout(read=SHORT_SECONDS, total=SHORT_SECONDS * 3))


@pytest.mark.usefixtures("native_library")
def test_the_read_timer_stops_at_a_redirect(silent_port: int, slow_server: SlowServer) -> None:
    # The next hop never gets a connection, and that time is not the read timer's to count.
    url = slow_server.url(f"/redirect?to=https://127.0.0.1:{silent_port}/")

    with mitto.Session() as session, takes_at_least(ROOMY_SECONDS), pytest.raises(mitto.RequestTimeoutError):
        session.get(url, timeout=Timeout(read=SHORT_SECONDS, total=ROOMY_SECONDS))


@pytest.mark.usefixtures("native_library")
def test_the_read_timer_runs_while_waiting_for_the_body(slow_server: SlowServer) -> None:
    with (
        mitto.Session() as session,
        takes_at_least(SHORT_SECONDS),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(slow_server.url("/stops-halfway"), timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS))

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_bytes_that_keep_coming_keep_the_read_timer_from_running_out(slow_server: SlowServer) -> None:
    # Longer than the timer in all, but with shorter waits.
    assert ROOMY_SECONDS < PAUSE_SECONDS * TRICKLE_CHUNKS

    with mitto.Session() as session:
        response = session.get(slow_server.url("/trickle"), timeout=Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS))

    assert response.content == BODY * TRICKLE_CHUNKS


@pytest.mark.usefixtures("native_library")
def test_bytes_that_a_decoder_holds_back_keep_the_read_timer_from_running_out(slow_server: SlowServer) -> None:
    # The decoder gives nothing back for most of these bytes, so no read completes during that time.
    with mitto.Session() as session:
        response = session.get(
            slow_server.url("/gzip-trickle"), timeout=Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS)
        )

    assert response.content == BODY


def test_a_caller_who_has_not_started_to_read_does_not_run_the_read_timer(
    native_library: ctypes.CDLL, slow_server: SlowServer
) -> None:
    timeout = Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS)

    with _native.Session() as session, session.start(slow_server.url("/ok"), timeout=timeout) as request:
        assert isinstance(request.next_event(WELL_BEFORE_LONG_SECONDS), ResponseStarted)
        time.sleep(LONGER_THAN_A_STALLED_READ_SECONDS)

        assert request.next_event(0) is None
        # The first read asks for the body, which then comes.
        assert request.read() is None
        assert isinstance(request.next_event(WELL_BEFORE_LONG_SECONDS), BodyAvailable)
        assert request.read() == BODY


def test_a_caller_who_stops_reading_does_not_run_the_read_timer(
    native_library: ctypes.CDLL, slow_server: SlowServer
) -> None:
    timeout = Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS)

    with _native.Session() as session, session.start(slow_server.url("/trickle"), timeout=timeout) as request:
        assert isinstance(request.next_event(WELL_BEFORE_LONG_SECONDS), ResponseStarted)
        # The server pauses first, so this asks for bytes that have not come.
        assert request.read() is None
        assert isinstance(request.next_event(WELL_BEFORE_LONG_SECONDS), BodyAvailable)
        assert request.read() == BODY
        time.sleep(LONGER_THAN_A_STALLED_READ_SECONDS)

        assert request.next_event(0) is None


@pytest.mark.usefixtures("native_library")
def test_the_total_time_runs_while_bytes_keep_coming(slow_server: SlowServer) -> None:
    with (
        mitto.Session() as session,
        takes_at_least(SHORT_SECONDS * 3),
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(slow_server.url("/endless-trickle"), timeout=Timeout(read=LONG_SECONDS, total=SHORT_SECONDS * 3))

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_total_time_counts_all_the_redirects(silent_port: int, slow_server: SlowServer) -> None:
    url = slow_server.url(f"/redirect?to=http://127.0.0.1:{silent_port}/")

    with mitto.Session() as session, takes_at_least(SHORT_SECONDS), pytest.raises(mitto.RequestTimeoutError):
        session.get(url, timeout=SHORT_SECONDS)


@pytest.mark.usefixtures("native_library")
def test_a_response_within_the_time_is_a_response(slow_server: SlowServer) -> None:
    with mitto.Session() as session:
        response = session.get(
            slow_server.url("/ok"), timeout=Timeout(connect=LONG_SECONDS, read=LONG_SECONDS, total=LONG_SECONDS)
        )

    assert response.content == BODY


@pytest.mark.usefixtures("native_library")
def test_a_session_has_no_timer_by_default(slow_server: SlowServer) -> None:
    with mitto.Session() as session:
        assert session.get(slow_server.url("/delayed")).content == BODY


@pytest.mark.usefixtures("native_library")
def test_a_sessions_timeout_is_the_default(silent_port: int) -> None:
    with (
        mitto.Session(timeout=SHORT_SECONDS) as session,
        takes_at_least(SHORT_SECONDS),
        pytest.raises(mitto.RequestTimeoutError),
    ):
        session.get(f"http://127.0.0.1:{silent_port}/")


@pytest.mark.usefixtures("native_library")
def test_a_session_takes_a_timeout_with_each_timer(silent_port: int) -> None:
    timeout = Timeout(connect=SHORT_SECONDS, total=LONG_SECONDS)

    with mitto.Session(timeout=timeout) as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.get(f"https://127.0.0.1:{silent_port}/")

    assert caught.value.net_error == ERR_CONNECTION_TIMED_OUT


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("timeout", [LONG_SECONDS, Timeout(total=LONG_SECONDS), Timeout(), None])
def test_a_requests_timeout_replaces_the_sessions(slow_server: SlowServer, timeout: float | Timeout | None) -> None:
    with mitto.Session(timeout=SHORT_SECONDS) as session:
        assert session.get(slow_server.url("/delayed"), timeout=timeout).content == BODY


@pytest.mark.usefixtures("native_library")
def test_get_takes_a_timeout(silent_port: int) -> None:
    with takes_at_least(SHORT_SECONDS), pytest.raises(mitto.RequestTimeoutError):
        mitto.get(f"http://127.0.0.1:{silent_port}/", timeout=SHORT_SECONDS)


@pytest.mark.parametrize("seconds", [0, -1, math.nan, math.inf, MAX_TIMEOUT_SECONDS + 1])
@pytest.mark.parametrize("name", ["connect", "read", "total"])
def test_a_timer_that_is_not_a_positive_number_is_refused(name: str, seconds: float) -> None:
    with pytest.raises(ValueError, match=name):
        Timeout(**{name: seconds})


def test_each_timer_is_named() -> None:
    with pytest.raises(TypeError):
        Timeout(1, 2, 3)  # pyright: ignore[reportCallIssue]


@pytest.mark.parametrize("seconds", [0, -1, math.nan, math.inf])
def test_a_number_that_is_not_positive_is_refused_before_a_request(seconds: float) -> None:
    with pytest.raises(ValueError, match="total"):
        build_request(load_persona(ENGINE_RELEASE_PERSONA), "GET", "http://example.test/", timeout=seconds)


def test_a_sessions_timeout_is_refused_before_the_library_loads(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(ValueError, match="total"):
        mitto.Session(timeout=0)


def test_a_timer_becomes_whole_milliseconds_rounded_up() -> None:
    options = _native.timeout_options(Timeout(connect=0.0001, read=1.5, total=MAX_TIMEOUT_SECONDS))

    assert options == {
        "connect_timeout_ms": 1,
        "read_timeout_ms": 1500,
        "total_timeout_ms": MAX_TIMEOUT_SECONDS * 1000,
    }
    assert options["total_timeout_ms"] <= MAX_TIMEOUT_MS


def test_no_timer_sets_no_option() -> None:
    assert _native.timeout_options(Timeout()) == {}


def start_with_options(library: ctypes.CDLL, session: _native.Session, options: bytes) -> tuple[int, Any]:
    """Call mitto_request_start with options as they are, and return its result and handle."""
    handle = ctypes.POINTER(_native.MittoRequest)()
    # The private handle is the point: the public API refuses most of these before they get here.
    native_session = session._handle  # pyright: ignore[reportPrivateUsage]
    error: int = library.mitto_request_start(
        native_session, b"http://127.0.0.1:9/", None, 0, options, len(options), None, 0, ctypes.byref(handle)
    )
    return error, handle


@pytest.mark.parametrize("key", ["connect_timeout_ms", "read_timeout_ms", "total_timeout_ms"])
@pytest.mark.parametrize("value", ["0", "-1", "1.5", '"1"', "true", "null", str(MAX_TIMEOUT_MS + 1), "[1]"])
def test_a_native_timer_that_is_not_valid_is_refused(native_library: ctypes.CDLL, key: str, value: str) -> None:
    options = f'{{"version": {_native.REQUEST_OPTIONS_VERSION}, "{key}": {value}}}'.encode()

    with _native.Session() as session:
        error, handle = start_with_options(native_library, session, options)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


@pytest.mark.parametrize("key", ["connect_timeout_ms", "read_timeout_ms", "total_timeout_ms"])
@pytest.mark.parametrize("value", [1, MAX_TIMEOUT_MS])
def test_a_native_timer_takes_any_positive_32_bit_number(native_library: ctypes.CDLL, key: str, value: int) -> None:
    options = f'{{"version": {_native.REQUEST_OPTIONS_VERSION}, "{key}": {value}}}'.encode()

    with _native.Session() as session:
        error, handle = start_with_options(native_library, session, options)
        native_library.mitto_request_release(handle)

    assert error == 0


@contextmanager
def _server_of_one_slow_upload(
    read_size: int, pause_seconds: float, answer: bytes = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
) -> Generator[int]:
    """Accept one connection, read a body slowly and give `answer` once it has all come. Yield the port."""

    def serve(listener: socket.socket) -> None:
        connection, _ = listener.accept()
        with connection:
            head = b""
            while b"\r\n\r\n" not in head:
                head += connection.recv(READ_A_HEAD_SIZE)
            _, _, read_ahead = head.partition(b"\r\n\r\n")
            size = int(next(line for line in head.split(b"\r\n") if line.startswith(b"Content-Length:")).split(b":")[1])
            remaining = size - len(read_ahead)
            while remaining > 0:
                time.sleep(pause_seconds)
                remaining -= len(connection.recv(read_size))
            connection.sendall(answer)

    with socket.create_server(("127.0.0.1", 0)) as listener:
        thread = threading.Thread(target=serve, args=(listener,), daemon=True)
        thread.start()
        yield listener.getsockname()[1]
        thread.join(LONG_SECONDS)


@pytest.mark.usefixtures("native_library")
def test_a_body_that_keeps_moving_does_not_run_out_the_read_timer() -> None:
    # The socket buffers of a loopback connection take a few megabytes, and this reads the rest in about four seconds.
    body = bytes(LARGE_BODY_SIZE)
    with _server_of_one_slow_upload(READ_SIZE_OF_A_SLOW_SERVER, PAUSE_SECONDS) as port, mitto.Session() as session:
        started_at = time.monotonic()
        response = session.post(
            f"http://127.0.0.1:{port}/", data=body, timeout=Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS)
        )

    assert response.status == 200
    assert time.monotonic() - started_at > ROOMY_SECONDS


@pytest.mark.usefixtures("native_library")
def test_a_body_that_keeps_moving_after_a_redirect_does_not_run_out_the_read_timer() -> None:
    # //net keeps the upload progress of the first hop, which a read timer must not take for a stalled server.
    body = bytes(LARGE_BODY_SIZE)
    with (
        _server_of_one_slow_upload(READ_SIZE_OF_A_SLOW_SERVER, PAUSE_SECONDS) as slow_port,
        _server_of_one_slow_upload(
            LARGE_BODY_SIZE,
            0,
            f"HTTP/1.1 307 Redirect\r\nLocation: http://127.0.0.1:{slow_port}/\r\nContent-Length: 0\r\n\r\n".encode(),
        ) as port,
        mitto.Session() as session,
    ):
        response = session.post(
            f"http://127.0.0.1:{port}/", data=body, timeout=Timeout(read=ROOMY_SECONDS, total=LONG_SECONDS)
        )

    assert response.status == 200
    assert len(response.history) == 1


def _slow_parts(count: int, pause_seconds: float) -> Iterator[bytes]:
    for _ in range(count):
        time.sleep(pause_seconds)
        yield BODY


@pytest.mark.usefixtures("native_library")
def test_a_wait_for_the_next_part_of_a_stream_does_not_run_out_the_read_timer() -> None:
    parts = 4
    # Each pause is longer than the read timer allows, and all of them far longer than a turn that notices moving bytes.
    with _server_of_one_slow_upload(READ_A_HEAD_SIZE, 0) as port, mitto.Session() as session:
        response = session.post(
            f"http://127.0.0.1:{port}/",
            data=_slow_parts(parts, SHORT_SECONDS * 3),
            headers={"Content-Length": str(len(BODY) * parts)},
            timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS),
        )

    assert response.status == 200


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_a_wait_for_the_next_part_of_a_stream_does_not_run_out_the_read_timer_over(protocol: str) -> None:
    with CaptureServer() as server:
        run = run_of("fetch_post_stream", protocol)
        assert run is not None
        with mitto.Session(_settings=session_settings(server, run)) as session:
            response = session.post(
                server.url(run.route, host=run.host, path="/case/fetch-post-stream"),
                data=_slow_parts(3, SHORT_SECONDS * 3),
                timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS),
            )

    assert response.status == 200
    assert response.protocol == protocol


@pytest.mark.usefixtures("native_library")
def test_the_read_timer_covers_the_wait_for_an_answer_after_the_last_part_of_a_stream(silent_port: int) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.post(
            f"http://127.0.0.1:{silent_port}/",
            data=[BODY],
            headers={"Content-Length": str(len(BODY))},
            timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS),
        )

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_a_stream_that_a_server_stops_taking_runs_out_the_read_timer(silent_port: int) -> None:
    parts = LARGE_BODY_SIZE // READ_SIZE_OF_A_SLOW_SERVER
    with mitto.Session() as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.post(
            f"http://127.0.0.1:{silent_port}/",
            data=[bytes(READ_SIZE_OF_A_SLOW_SERVER)] * parts,
            headers={"Content-Length": str(LARGE_BODY_SIZE)},
            timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS),
        )

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_the_total_timer_covers_a_wait_for_the_next_part_of_a_stream(silent_port: int) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.post(
            f"http://127.0.0.1:{silent_port}/",
            data=_slow_parts(100, SHORT_SECONDS),
            headers={"Content-Length": str(len(BODY) * 100)},
            timeout=Timeout(total=SHORT_SECONDS * 3),
        )

    assert caught.value.net_error == ERR_TIMED_OUT


@pytest.mark.usefixtures("native_library")
def test_a_body_that_a_server_stops_taking_runs_out_the_read_timer(silent_port: int) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestTimeoutError) as caught:
        session.post(
            f"http://127.0.0.1:{silent_port}/",
            data=bytes(LARGE_BODY_SIZE),
            timeout=Timeout(read=SHORT_SECONDS, total=LONG_SECONDS),
        )

    assert caught.value.net_error == ERR_TIMED_OUT
