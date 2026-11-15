"""Tests of a body that goes a part at a time, each way, through the public API. They need a native library.

Local servers stand in for the Internet. The capture server speaks HTTP/2 and HTTP/3, with no flow control of its own, so a
body sent to it stays small here. tests/test_big_transfers.py moves large ones.
"""

import ctypes
import gzip
import http.server
import io
import json
import select
import socket
import sys
import threading
from collections.abc import Callable, Generator, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, urlencode, urlsplit

import pytest

import mitto
from mitto import _native
from mitto._body import StreamBody
from mitto._native import BodyWanted, NativeRequest
from tests.local_server import LocalHTTPServer, refused_port
from tools.capture import CASES, HTTP1, HTTP2, HTTP3, CaptureServer, normal_form
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("native_library", "both_apis")

ERR_INVALID_ARGUMENT = -4
ERR_UPLOAD_FILE_CHANGED = -14
ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED = -25
ERR_ABORTED = -3

BODY = b"the whole body of a response"
PARTS = [b"first ", b"second ", b"third"]
WHOLE = b"".join(PARTS)
EVENT_SECONDS = 30.0

# More than the native core keeps of a stream for another attempt.
MORE_THAN_IS_KEPT = 1024 * 1024
# Less than that, and less than the window of a capture server that opens none.
LESS_THAN_IS_KEPT = 16 * 1024


@dataclass(frozen=True, slots=True)
class Received:
    method: str
    path: str
    headers: dict[str, str]
    body: bytes


class StreamServer(LocalHTTPServer):
    """An HTTP/1.1 server that records what it receives, and whose paths answer in a way that tests can watch."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), StreamHandler)
        self.received: list[Received] = []
        self.send_the_body = threading.Event()
        self.reader_left = threading.Event()

    def url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.server_port}{path}"


class StreamHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast(StreamServer, self.server)
        length = int(self.headers.get("Content-Length", 0))
        server.received.append(
            Received(self.command, self.path, {k.lower(): v for k, v in self.headers.items()}, self.rfile.read(length))
        )
        url = urlsplit(self.path)
        match url.path:
            case "/body":
                self.send_body(BODY)
            case "/held":
                self.send_head(len(BODY))
                self.wfile.flush()
                server.send_the_body.wait(timeout=EVENT_SECONDS)
                self.wfile.write(BODY)
            case "/stops-halfway":
                self.send_head(len(BODY) * 2)
                self.wfile.write(BODY)
                self.close_connection = True
            case "/gzip":
                encoded = gzip.compress(BODY)
                self.send_response(200)
                self.send_header("Content-Encoding", "gzip")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)
            case "/endless":
                self.send_head(1 << 40)
                try:
                    while True:
                        self.wfile.write(bytes(64 * 1024))
                except OSError:
                    server.reader_left.set()
                    self.close_connection = True
            case "/misdirected":
                self.send_response(421)
                self.send_header("Content-Length", "0")
                self.end_headers()
            case "/redirect":
                query = parse_qs(url.query)
                self.send_response(int(query["status"][0]))
                self.send_header("Location", query["to"][0])
                self.send_header("Content-Length", "0")
                self.end_headers()
            case _:
                self.send_body(b"")

    do_POST = do_GET  # noqa: N815

    def send_head(self, size: int) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()

    def send_body(self, body: bytes) -> None:
        self.send_head(len(body))
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[StreamServer]:
    with StreamServer() as stream_server:
        threading.Thread(target=stream_server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield stream_server
        stream_server.send_the_body.set()
        stream_server.shutdown()


# Reading a response.


def test_the_head_comes_before_the_body(server: StreamServer) -> None:
    with mitto.Session() as session, session.stream("GET", server.url("/held")) as response:
        assert response.status == 200
        assert response.url == server.url("/held")
        assert response.protocol == "http/1.1"
        assert response.headers["Content-Length"] == str(len(BODY))
        assert response.history == ()
        server.send_the_body.set()
        assert response.read() == BODY


def test_iter_bytes_gives_the_whole_body(server: StreamServer) -> None:
    with mitto.Session() as session, session.stream("GET", server.url("/body")) as response:
        assert b"".join(response.iter_bytes()) == BODY


def test_read_gives_the_rest_of_what_iter_bytes_has_not_given(server: StreamServer) -> None:
    with mitto.Session() as session, session.stream("GET", server.url("/endless")) as endless:
        first = next(endless.iter_bytes())
        second = next(endless.iter_bytes())
        assert first
        assert second

    with mitto.Session() as session, session.stream("GET", server.url("/body")) as response:
        start = next(response.iter_bytes())
        assert start
        assert start + response.read() == BODY
        assert response.read() == b""
        assert list(response.iter_bytes()) == []


def test_the_body_comes_after_content_decoding(server: StreamServer) -> None:
    with mitto.Session() as session, session.stream("GET", server.url("/gzip")) as response:
        assert response.read() == BODY


def test_a_redirect_is_in_the_history(server: StreamServer) -> None:
    url = server.url("/redirect?status=302&to=/body")
    with mitto.Session() as session, session.stream("GET", url) as response:
        assert [(hop.status, hop.url) for hop in response.history] == [(302, url)]
        assert response.url == server.url("/body")
        assert response.read() == BODY


def test_a_redirect_is_the_response_if_asked(server: StreamServer) -> None:
    url = server.url("/redirect?status=302&to=/body")
    with mitto.Session() as session, session.stream("GET", url, follow_redirects=False) as response:
        assert response.status == 302
        assert response.read() == b""


def test_leaving_early_cancels_the_request(server: StreamServer) -> None:
    with mitto.Session() as session:
        with session.stream("GET", server.url("/endless")) as response:
            assert next(response.iter_bytes())

        assert server.reader_left.wait(timeout=EVENT_SECONDS)
        assert session.get(server.url("/body")).content == BODY


def test_leaving_through_an_error_cancels_the_request(server: StreamServer) -> None:
    with mitto.Session() as session, pytest.raises(KeyError), session.stream("GET", server.url("/endless")):
        raise KeyError

    assert server.reader_left.wait(timeout=EVENT_SECONDS)


def test_a_slow_reader_makes_the_server_wait(server: StreamServer) -> None:
    with mitto.Session() as session, session.stream("GET", server.url("/endless")) as response:
        assert next(response.iter_bytes())
        assert not server.reader_left.wait(timeout=0.1)


def _read_all(response: mitto.ResponseStream) -> object:
    return response.read()


def _iterate_all(response: mitto.ResponseStream) -> object:
    return list(response.iter_bytes())


@pytest.mark.parametrize("read", [_read_all, _iterate_all], ids=["read", "iter_bytes"])
def test_reading_after_the_with_statement_raises(
    server: StreamServer, read: Callable[[mitto.ResponseStream], Any]
) -> None:
    with mitto.Session() as session:
        with session.stream("GET", server.url("/endless")) as response:
            pass

        with pytest.raises(mitto.RequestError):
            read(response)


def test_a_body_that_stops_halfway_raises_from_what_reads_it(server: StreamServer) -> None:
    with (
        mitto.Session() as session,
        session.stream("GET", server.url("/stops-halfway")) as response,
        pytest.raises(mitto.RequestError),
    ):
        response.read()


def test_no_connection_raises_when_the_with_statement_starts() -> None:
    port = refused_port()
    with (
        mitto.Session() as session,
        pytest.raises(mitto.ConnectionFailedError),
        session.stream("GET", f"http://127.0.0.1:{port}/"),
    ):
        pytest.fail("the block must not run")


def test_arguments_are_checked_as_for_any_request(server: StreamServer) -> None:
    with (
        mitto.Session() as session,
        pytest.raises(ValueError, match="RequestKind"),
        session.stream("GET", server.url("/body"), kind="nothing"),
    ):
        pytest.fail("the block must not run")


def test_the_timeout_of_the_session_applies(server: StreamServer) -> None:
    with (
        mitto.Session(timeout=0.2) as session,
        pytest.raises(mitto.RequestTimeoutError),
        session.stream("GET", server.url("/held")) as response,
    ):
        response.read()


def _capture_url(capture: CaptureServer, protocol: str, case_name: str) -> tuple[str, Any, Any]:
    """Return the path of a Recording Case over a protocol, the settings that reach it and the case."""
    run = run_of(case_name, protocol)
    assert run is not None
    case = next(case for case in CASES if case.name == case_name)
    return capture.url(run.route, host=run.host, path=case.path), session_settings(capture, run), case


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_response_arrives_over_each_protocol(protocol: str) -> None:
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, protocol, "fetch_post_stream")
        with mitto.Session(_settings=settings) as session:
            with session.stream("GET", url) as response:
                assert response.protocol == protocol
                assert response.status == 200
                body = response.read()
            assert session.get(url).content == body


# Sending a body.


def _recorded_request(capture: CaptureServer, protocol: str, case: Any) -> Any:
    run = run_of(case.name, protocol)
    assert run is not None
    form = normal_form(capture.recording(run.route, host=run.host), [case])
    return cast(Any, form["requests"])[case.name]


def _sent(protocol: str, **options: Any) -> Any:
    """Send a POST with these options to the capture server and return what it received, in the normal form."""
    with CaptureServer() as capture:
        url, settings, case = _capture_url(capture, protocol, "fetch_post_stream")
        with mitto.Session(_settings=settings) as session:
            response = session.post(url, kind="fetch", **options)
        assert response.status == 200
        return _recorded_request(capture, protocol, case)


def _names(request: Any) -> list[str]:
    return [line.partition(":")[0].lower() for line in request["headers"] if not line.startswith(":")]


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_an_iterable_goes_in_parts_with_no_length(protocol: str) -> None:
    request = _sent(protocol, data=PARTS)

    assert request["body"] == WHOLE.decode()
    assert "content-length" not in _names(request)
    assert "transfer-encoding" not in _names(request)
    assert "content-type" not in _names(request)
    assert "origin" in _names(request)


def test_each_part_is_a_frame_of_its_own_and_the_end_is_one_more_over_http2() -> None:
    request = _sent(HTTP2, data=PARTS)

    assert request["later_frames"] == [
        {"type": "DATA", "flags": []},
        {"type": "DATA", "flags": []},
        {"type": "DATA", "flags": []},
        {"type": "DATA", "flags": ["END_STREAM"]},
    ]


def test_each_part_is_a_frame_of_its_own_over_http3() -> None:
    assert _sent(HTTP3, data=PARTS)["later_frames"] == [{"type": "DATA"}, {"type": "DATA"}, {"type": "DATA"}]


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_a_part_larger_than_the_native_core_holds_goes_whole(protocol: str) -> None:
    # Over HTTP/2 this has to stay within the window of the capture server, which opens no more.
    part = bytes(range(256)) * (LESS_THAN_IS_KEPT // 256)
    assert _sent(protocol, data=[part, part, part])["body"].encode("latin-1") == part * 3


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_an_empty_part_is_nothing(protocol: str) -> None:
    assert _sent(protocol, data=[b"", b"part", b""])["body"] == "part"


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_an_empty_iterable_ends_with_no_body(protocol: str) -> None:
    request = _sent(protocol, data=[])

    assert "body" not in request
    assert "content-length" not in _names(request)


def test_an_empty_iterable_has_an_empty_end_over_http2() -> None:
    request = _sent(HTTP2, data=[])

    assert "END_STREAM" not in request["flags"]
    assert request["later_frames"] == [{"type": "DATA", "flags": ["END_STREAM"]}]


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_file_goes_with_a_length_over_each_protocol(protocol: str) -> None:
    request = _sent(protocol, data=io.BytesIO(WHOLE))

    assert request["body"] == WHOLE.decode()
    assert f"content-length: {len(WHOLE)}" in [line.lower() for line in request["headers"]]
    assert "transfer-encoding" not in _names(request)


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_length_goes_where_it_goes_for_a_body_in_memory(protocol: str) -> None:
    in_memory = _sent(protocol, data=WHOLE)
    from_file = _sent(protocol, data=io.BytesIO(WHOLE))

    assert _names(from_file) == _names(in_memory)


def test_a_file_on_disk_goes_from_where_it_stands(server: StreamServer, tmp_path: Path) -> None:
    path = tmp_path / "body"
    path.write_bytes(b"skipped, then the rest")
    with path.open("rb") as file, mitto.Session() as session:
        file.seek(len(b"skipped, "))
        response = session.post(server.url("/"), data=file)

    assert response.status == 200
    received = server.received[-1]
    assert received.body == b"then the rest"
    assert received.headers["content-length"] == str(len(b"then the rest"))


def test_an_empty_file_goes_as_a_length_of_zero(server: StreamServer) -> None:
    with mitto.Session() as session:
        assert session.post(server.url("/"), data=io.BytesIO()).status == 200

    assert server.received[-1].headers["content-length"] == "0"
    assert server.received[-1].body == b""


def test_a_caller_gives_the_length_of_an_iterable(server: StreamServer) -> None:
    with mitto.Session() as session:
        response = session.post(server.url("/"), data=PARTS, headers={"content-length": str(len(WHOLE))})

    assert response.status == 200
    assert server.received[-1].body == WHOLE
    assert server.received[-1].headers["content-length"] == str(len(WHOLE))
    assert "transfer-encoding" not in server.received[-1].headers


def test_a_length_of_none_is_no_length(server: StreamServer) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.post(server.url("/"), data=PARTS, headers={"Content-Length": None})

    assert caught.value.name == "ERR_ALPN_NEGOTIATION_FAILED"


@pytest.mark.parametrize("value", ["", "x", "-1", "1.5", "٣"])
def test_a_length_that_is_no_number_is_refused(server: StreamServer, value: str) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="number of bytes"):
        session.post(server.url("/"), data=PARTS, headers={"Content-Length": value})


def test_a_length_past_what_the_native_core_takes_is_refused(server: StreamServer) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.post(server.url("/"), data=PARTS, headers={"Content-Length": str(2**64)})

    assert caught.value.net_error == ERR_INVALID_ARGUMENT


@pytest.mark.parametrize(("size", "message"), [(1, "more bytes"), (1000, "fewer than")])
def test_an_iterable_that_does_not_keep_to_its_length_is_refused(server: StreamServer, size: int, message: str) -> None:
    with mitto.Session() as session:
        with pytest.raises(ValueError, match=message):
            session.post(server.url("/"), data=PARTS, headers={"Content-Length": str(size)})

        assert session.get(server.url("/body")).content == BODY


def test_an_iterable_with_a_length_of_zero_has_to_give_nothing(server: StreamServer) -> None:
    # //net reads nothing of a body with this size, and the check still has to come.
    with mitto.Session() as session:
        with pytest.raises(ValueError, match="more bytes"):
            session.post(server.url("/"), data=[b"part"], headers={"Content-Length": "0"})

        response = session.post(server.url("/"), data=[b"", b""], headers={"Content-Length": "0"})

    assert response.status == 200
    assert server.received[-1].body == b""


def test_an_iterable_with_a_length_of_zero_gets_to_end(server: StreamServer) -> None:
    ended = False

    def parts() -> Iterator[bytes]:
        nonlocal ended
        yield from ()
        ended = True

    with mitto.Session() as session:
        session.post(server.url("/"), data=parts(), headers={"Content-Length": "0"})

    assert ended


def test_a_stream_that_gives_less_than_its_size_fails_the_request(server: StreamServer) -> None:
    # What a file that becomes shorter on the way does, which encode_body() does not check.
    stream = StreamBody(iter([memoryview(b"half")]), len(b"half") * 2)
    with _native.Session() as native:
        with (
            native.start(server.url("/"), method="POST", body=stream) as request,
            pytest.raises(mitto.RequestError) as caught,
        ):
            _native.wait_for_response(request)

        assert caught.value.net_error == ERR_UPLOAD_FILE_CHANGED
        assert native.request(server.url("/body")).body == BODY


@pytest.mark.parametrize("data", [PARTS, iter(PARTS), (part for part in PARTS)], ids=["list", "iterator", "generator"])
def test_a_stream_of_no_known_size_is_refused_over_http1(server: StreamServer, data: Any) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.post(server.url("/"), data=data)

    assert caught.value.name == "ERR_ALPN_NEGOTIATION_FAILED"
    assert server.received == []


def test_a_refused_stream_is_not_touched(server: StreamServer) -> None:
    started = False

    def parts() -> Iterator[bytes]:
        nonlocal started
        started = True
        yield b"part"

    with mitto.Session() as session, pytest.raises(mitto.RequestError):
        session.post(server.url("/"), data=parts())

    assert not started


def test_a_stream_of_no_known_size_is_refused_over_http1_with_tls() -> None:
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, HTTP1, "fetch_post_stream")
        with (
            mitto.Session(_settings=settings) as session,
            pytest.raises(mitto.RequestError) as caught,
        ):
            session.post(url, data=PARTS)
        recording = capture.recording(HTTP1)

    assert caught.value.name == "ERR_ALPN_NEGOTIATION_FAILED"
    assert not any(connection.requests for connection in recording.connections)


def test_a_refusal_leaves_the_session_working(server: StreamServer) -> None:
    with mitto.Session() as session:
        with pytest.raises(mitto.RequestError):
            session.post(server.url("/"), data=PARTS)

        assert session.post(server.url("/"), data=b"bytes").status == 200


def _parts_then_failure() -> Iterator[bytes]:
    yield b"part"
    raise KeyError


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_what_an_iterable_raises_reaches_the_caller_and_ends_the_request(protocol: str) -> None:
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, protocol, "fetch_post_stream")
        with mitto.Session(_settings=settings) as session:
            with pytest.raises(KeyError):
                session.post(url, data=_parts_then_failure())

            assert session.get(url).status == 200


def test_a_request_that_failed_on_the_callers_side_does_not_look_finished_to_the_server() -> None:
    with CaptureServer() as capture:
        url, settings, case = _capture_url(capture, HTTP2, "fetch_post_stream")
        with mitto.Session(_settings=settings) as session:
            with pytest.raises(KeyError):
                session.post(url, data=_parts_then_failure())
            # The server handles a connection's frames in order, so this one is past what the failed request sent.
            session.get(url)
        request = _recorded_request(capture, HTTP2, case)

    frames = [frame["type"] for frame in request["later_frames"]]
    assert frames[-1] == "RST_STREAM"
    assert not any("END_STREAM" in frame.get("flags", []) for frame in request["later_frames"])


def test_what_an_iterable_raises_when_entering_a_stream_reaches_the_caller() -> None:
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, HTTP2, "fetch_post_stream")
        with (
            mitto.Session(_settings=settings) as session,
            pytest.raises(KeyError),
            session.stream("POST", url, data=_parts_then_failure()),
        ):
            pytest.fail("the block must not run")


def test_a_part_that_is_not_bytes_reaches_the_caller() -> None:
    parts: Any = ["text"]
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, HTTP2, "fetch_post_stream")
        with mitto.Session(_settings=settings) as session, pytest.raises(TypeError, match="has to give bytes"):
            session.post(url, data=parts)


def test_a_file_in_text_mode_is_refused(server: StreamServer) -> None:
    file: Any = io.StringIO("text")
    with mitto.Session() as session, pytest.raises(TypeError, match="binary"):
        session.post(server.url("/"), data=file)


def test_a_stream_is_refused_with_get(server: StreamServer) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="GET"):
        session.request("GET", server.url("/"), data=PARTS)


def test_a_stream_is_refused_with_a_kind_that_chrome_sends_no_body_with(server: StreamServer) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="script"):
        session.post(server.url("/"), data=PARTS, kind="script")


def test_a_caller_gives_a_stream_its_content_type(server: StreamServer) -> None:
    with mitto.Session() as session:
        session.post(server.url("/"), data=io.BytesIO(WHOLE), headers={"Content-Type": "text/plain"})

    assert server.received[-1].headers["content-type"] == "text/plain"


def test_both_ways_at_once(server: StreamServer) -> None:
    with (
        mitto.Session() as session,
        session.stream("POST", server.url("/body"), data=io.BytesIO(WHOLE)) as response,
    ):
        assert response.read() == BODY

    assert server.received[-1].body == WHOLE


# A redirect of a stream, which goes only once.


@pytest.mark.parametrize("status", [301, 302, 307, 308])
def test_a_redirect_fails_a_stream_as_fetch_fails_it(server: StreamServer, status: int) -> None:
    url = server.url(f"/redirect?status={status}&to=/body")
    with mitto.Session() as session:
        with pytest.raises(mitto.RequestError) as caught:
            session.post(url, data=io.BytesIO(WHOLE))

        assert caught.value.net_error == ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED
        assert [received.path for received in server.received] == [urlsplit(url).path + "?" + urlsplit(url).query]
        assert session.get(server.url("/body")).content == BODY


def test_a_303_drops_a_stream_as_it_drops_any_body(server: StreamServer) -> None:
    url = server.url("/redirect?status=303&to=/body")
    with mitto.Session() as session:
        response = session.post(url, data=io.BytesIO(WHOLE))

    assert response.content == BODY
    assert [hop.status for hop in response.history] == [303]
    assert [received.method for received in server.received] == ["POST", "GET"]
    assert server.received[0].body == WHOLE
    assert server.received[1].body == b""
    assert "content-length" not in server.received[1].headers


@pytest.mark.parametrize("status", [301, 302, 303, 307, 308])
def test_a_redirect_after_a_303_has_no_stream_to_send_again(server: StreamServer, status: int) -> None:
    second = f"/redirect?status={status}&to=/body"
    url = server.url(f"/redirect?{urlencode({'status': 303, 'to': second})}")
    with mitto.Session() as session:
        response = session.post(url, data=io.BytesIO(WHOLE))

    assert response.content == BODY
    assert [hop.status for hop in response.history] == [303, status]
    assert [received.method for received in server.received] == ["POST", "GET", "GET"]


@pytest.mark.parametrize("status", [301, 302, 303, 307, 308])
def test_a_redirect_that_is_the_response_is_no_trouble(server: StreamServer, status: int) -> None:
    url = server.url(f"/redirect?status={status}&to=/body")
    with mitto.Session() as session:
        response = session.post(url, data=io.BytesIO(WHOLE), follow_redirects=False)

    assert response.status == status
    assert len(server.received) == 1


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_a_redirect_fails_a_stream_of_no_known_size(protocol: str) -> None:
    with CaptureServer() as capture:
        url, settings, _ = _capture_url(capture, protocol, "redirect_fetch_1")
        with mitto.Session(_settings=settings) as session, pytest.raises(mitto.RequestError) as caught:
            session.post(url, data=PARTS)

    assert caught.value.net_error == ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED


def test_a_421_goes_back_to_the_caller_of_a_stream_as_chrome_does(server: StreamServer) -> None:
    # //net would send any other body again, on another connection.
    with mitto.Session() as session:
        assert session.post(server.url("/misdirected"), data=b"bytes").status == 421
        assert len(server.received) == 2
        server.received.clear()

        assert session.post(server.url("/misdirected"), data=io.BytesIO(WHOLE)).status == 421
        assert len(server.received) == 1


# Another attempt on a new connection, which //net makes when a reused one turns out to be gone.


@dataclass(slots=True)
class _RetryServer:
    """Answers once, keeps the connection and drops it at the next request. On the next connection it answers again."""

    port: int
    received: list[bytes]
    let_go: threading.Event


def _read_head(connection: socket.socket) -> tuple[bytes, bytes]:
    head = b""
    while b"\r\n\r\n" not in head:
        data = connection.recv(4096)
        if not data:
            break
        head += data
    before, _, after = head.partition(b"\r\n\r\n")
    return before, after


def _read_body(connection: socket.socket, head: bytes, read_ahead: bytes) -> bytes:
    lines = head.split(b"\r\n")
    size = next((int(line.split(b":")[1]) for line in lines if line.lower().startswith(b"content-length:")), 0)
    body = read_ahead
    while len(body) < size:
        data = connection.recv(65536)
        if not data:
            break
        body += data
    return body


@pytest.fixture
def retry_server() -> Generator[_RetryServer]:
    answer = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
    received: list[bytes] = []
    let_go = threading.Event()

    def serve(listener: socket.socket) -> None:
        first, _ = listener.accept()
        with first:
            head, read_ahead = _read_head(first)
            _read_body(first, head, read_ahead)
            first.sendall(answer)
            _read_head(first)
            let_go.wait(timeout=EVENT_SECONDS)
        try:
            second, _ = listener.accept()
        except OSError:
            return
        with second:
            head, read_ahead = _read_head(second)
            received.append(_read_body(second, head, read_ahead))
            second.sendall(answer)

    with socket.create_server(("127.0.0.1", 0)) as listener:
        thread = threading.Thread(target=serve, args=(listener,), daemon=True)
        thread.start()
        yield _RetryServer(listener.getsockname()[1], received, let_go)
        let_go.set()
    thread.join(EVENT_SECONDS)


def test_another_attempt_sends_a_small_stream_again(retry_server: _RetryServer) -> None:
    url = f"http://127.0.0.1:{retry_server.port}/"
    body = bytes(range(256)) * (LESS_THAN_IS_KEPT // 256)
    retry_server.let_go.set()

    with mitto.Session() as session:
        assert session.get(url).status == 200
        response = session.post(url, data=[body], headers={"Content-Length": str(len(body))})

    assert response.status == 200
    assert retry_server.received == [body]


def test_another_attempt_fails_a_stream_that_has_gone_too_far(retry_server: _RetryServer) -> None:
    url = f"http://127.0.0.1:{retry_server.port}/"
    part = bytes(64 * 1024)
    parts = MORE_THAN_IS_KEPT // len(part)

    def watched_parts() -> Iterator[bytes]:
        for index in range(parts):
            # By the time the caller gives this, //net has taken more than the native core keeps.
            if index == parts // 2:
                retry_server.let_go.set()
            yield part

    with mitto.Session() as session:
        assert session.get(url).status == 200
        with pytest.raises(mitto.RequestError) as caught:
            session.post(url, data=watched_parts(), headers={"Content-Length": str(MORE_THAN_IS_KEPT)})

    assert caught.value.net_error == ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED


# The C API, as asyncio will use it.


def _start_stream(session: _native.Session, url: str, size: int | None = None) -> NativeRequest:
    return session.start(url, method="POST", body=StreamBody(iter(()), size))


def _wait_for_wanted(request: NativeRequest) -> None:
    while not isinstance(event := request.next_event(EVENT_SECONDS), BodyWanted):
        assert event is not None
        assert not isinstance(event, _native.Finished), event


@pytest.mark.skipif(sys.platform == "win32", reason="a Session has no file descriptor on Windows")
def test_the_event_fd_says_when_a_stream_is_wanted(server: StreamServer) -> None:
    with _native.Session() as session:
        fd = session.event_fd()
        with _start_stream(session, server.url("/"), len(WHOLE)) as request:
            readable, _, _ = select.select([fd], [], [], EVENT_SECONDS)
            assert readable == [fd]
            assert session.next_ready() == request.identity
            assert isinstance(request.next_event(0), BodyWanted)
            assert session.next_ready() is None

            assert request.write_body(WHOLE) == len(WHOLE)
            readable, _, _ = select.select([fd], [], [], EVENT_SECONDS)
            assert readable == [fd]
            assert session.next_ready() == request.identity
            assert isinstance(request.next_event(0), BodyWanted)
            request.end_body()
            assert isinstance(_native.wait_for_response(request)[0], _native.ResponseStarted)

    assert server.received[-1].body == WHOLE


def test_a_stream_is_not_touched_if_no_connection_comes() -> None:
    started = False

    def parts() -> Iterator[bytes]:
        nonlocal started
        started = True
        yield b"part"

    with mitto.Session() as session, pytest.raises(mitto.ConnectionFailedError):
        session.post(f"http://127.0.0.1:{refused_port()}/", data=parts(), headers={"Content-Length": "4"})

    assert not started


def test_a_write_takes_what_the_native_core_holds_and_then_no_more(server: StreamServer) -> None:
    held = 64 * 1024
    size = held + 1
    with _native.Session() as session, _start_stream(session, server.url("/"), size) as request:
        assert request.write_body(bytes(held + 1)) == held
        # //net may have taken it all by now, with the next event on its way, so this is one of two answers.
        assert request.write_body(b"x") in {None, 1}


def test_a_write_past_the_size_is_refused(server: StreamServer) -> None:
    with (
        _native.Session() as session,
        _start_stream(session, server.url("/"), 2) as request,
        pytest.raises(mitto.RequestError) as caught,
    ):
        request.write_body(b"abc")

    assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_write_of_nothing_is_refused(server: StreamServer) -> None:
    with (
        _native.Session() as session,
        _start_stream(session, server.url("/")) as request,
        pytest.raises(mitto.RequestError) as caught,
    ):
        request.write_body(b"")

    assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_write_after_the_end_is_refused(server: StreamServer) -> None:
    with _native.Session() as session, _start_stream(session, server.url("/")) as request:
        request.end_body()
        with pytest.raises(mitto.RequestError) as caught:
            request.write_body(b"a")

    assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_second_end_is_refused(server: StreamServer) -> None:
    with _native.Session() as session, _start_stream(session, server.url("/")) as request:
        request.end_body()
        with pytest.raises(mitto.RequestError) as caught:
            request.end_body()

    assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_request_with_no_stream_takes_no_writes(server: StreamServer) -> None:
    with _native.Session() as session, session.start(server.url("/body")) as request:
        for call in (lambda: request.write_body(b"a"), request.end_body):
            with pytest.raises(mitto.RequestError) as caught:
                call()
            assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_request_that_has_finished_wants_nothing(server: StreamServer) -> None:
    with _native.Session() as session, _start_stream(session, server.url("/"), len(WHOLE)) as request:
        _wait_for_wanted(request)
        request.cancel()
        while not isinstance(event := request.next_event(EVENT_SECONDS), _native.Finished):
            assert event is not None
        assert event.net_error == ERR_ABORTED

        assert request.write_body(WHOLE) == 0
        request.end_body()


def test_a_closed_request_takes_no_writes(server: StreamServer) -> None:
    with _native.Session() as session:
        request = _start_stream(session, server.url("/"))
        request.close()
        for call in (lambda: request.write_body(b"a"), request.end_body):
            with pytest.raises(mitto.RequestError) as caught:
                call()
            assert caught.value.net_error == ERR_INVALID_ARGUMENT


def test_a_303_tells_a_later_write_that_nothing_is_wanted(server: StreamServer) -> None:
    url = server.url("/redirect?status=303&to=/body")
    with _native.Session() as session, _start_stream(session, url, len(WHOLE)) as request:
        _wait_for_wanted(request)
        assert request.write_body(WHOLE) == len(WHOLE)
        while not isinstance(event := request.next_event(EVENT_SECONDS), _native.ResponseStarted):
            assert not isinstance(event, _native.Finished), event

        assert event.status == 200
        assert request.write_body(b"x") == 0
        assert b"".join(_native.iter_body(request)) == BODY


def _start_with_options(
    library: ctypes.CDLL, session: _native.Session, options: dict[str, object], body: bytes | None = None
) -> tuple[int, Any]:
    encoded = json.dumps({"version": _native.REQUEST_OPTIONS_VERSION, **options}).encode()
    handle = ctypes.POINTER(_native.MittoRequest)()
    error = library.mitto_request_start(
        session._handle,  # pyright: ignore[reportPrivateUsage]
        b"http://127.0.0.1:1/",
        None,
        0,
        encoded,
        len(encoded),
        body,
        0 if body is None else len(body),
        ctypes.byref(handle),
    )
    return error, handle


@pytest.mark.parametrize(
    "options",
    [
        {"stream_body": 1},
        {"stream_body": "true"},
        {"stream_body_size": 1},
        {"stream_body": False, "stream_body_size": 1},
        {"stream_body": True, "stream_body_size": -1},
        {"stream_body": True, "stream_body_size": 1.5},
        {"stream_body": True, "stream_body_size": "1"},
        {"stream_body": True, "stream_body_size": 2**53 + 2},
        {"stream_body": True, "stream_body_size": None},
    ],
    ids=str,
)
def test_the_options_of_a_stream_are_checked(native_library: ctypes.CDLL, options: dict[str, object]) -> None:
    with _native.Session() as session:
        error, handle = _start_with_options(native_library, session, options)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


@pytest.mark.parametrize(
    "options",
    [
        {"stream_body": False},
        {"stream_body": True},
        {"stream_body": True, "stream_body_size": 0},
        {"stream_body": True, "stream_body_size": 5 * 2**30},
        {"stream_body": True, "stream_body_size": 2**53},
    ],
    ids=str,
)
def test_the_options_of_a_stream_are_taken(native_library: ctypes.CDLL, options: dict[str, object]) -> None:
    with _native.Session() as session:
        error, handle = _start_with_options(native_library, session, options)
        native_library.mitto_request_release(handle)

    assert error == 0


def test_a_stream_and_a_body_in_memory_do_not_go_together(native_library: ctypes.CDLL) -> None:
    with _native.Session() as session:
        error, handle = _start_with_options(native_library, session, {"stream_body": True}, b"bytes")

    assert error == ERR_INVALID_ARGUMENT
    assert not handle
