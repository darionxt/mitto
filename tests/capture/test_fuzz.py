"""Fuzz targets for the parsers in the capture kit, run through Hypothesis.

Each target takes arbitrary bytes and may only fail in the way its parser documents. They run as part of the normal
test suite, on a fixed set of inputs. To look for new failures, with new inputs and many more of them, run:

    uv run pytest tests/capture/test_fuzz.py --hypothesis-profile=fuzz

Most random bytes fail at the first check, so the inputs are mostly real traffic with mutations, and well-formed
structures with random content, which reach deeper.
"""

import contextlib
import json

from hypothesis import given
from hypothesis import strategies as st
from hypothesis.strategies import SearchStrategy

from tests.capture import samples
from tools.capture.client_hello import ClientHelloAssembler, ClientHelloRecords, parse_client_hello
from tools.capture.http1 import Http1Connection, parse_request_head
from tools.capture.http2 import PREFACE, Frame, Http2Connection, serialize_frame
from tools.capture.names import EXTENSIONS
from tools.capture.normal_form import RecordingError, normal_client_hello, normal_form
from tools.capture.page import KIND_PATHS, respond
from tools.capture.recording import ConnectionRecord, Recording, raw_form
from tools.capture.wire import ParseError


def fuzz_client_hello(chunks: list[bytes]) -> None:
    """Target: a TCP stream into the ClientHello parser, then the result into the normal form."""
    assembler = ClientHelloAssembler()
    with contextlib.suppress(ParseError):
        for chunk in chunks:
            found = assembler.feed(chunk)
            if found is not None:
                json.dumps(normal_client_hello(found))
                return


def fuzz_client_hello_message(message: bytes) -> None:
    """Target: a handshake message into the ClientHello parser, as QUIC will do without TLS records."""
    with contextlib.suppress(ParseError):
        parse_client_hello(message)


def _exercise_forms(connection: ConnectionRecord) -> None:
    """Whatever a connection managed to record must make it into both forms, or raise what normal_form documents."""
    recording = Recording(samples.HOST, samples.AUTHORITY, (connection,))
    json.dumps(raw_form(recording))
    with contextlib.suppress(RecordingError):
        json.dumps(normal_form(recording))


def _a_client_hello() -> ClientHelloRecords:
    found = ClientHelloAssembler().feed(samples.client_hello_records())
    assert found is not None
    return found


_CLIENT_HELLO = _a_client_hello()


def fuzz_http2(chunks: list[bytes]) -> None:
    """Target: the decrypted bytes of a connection into the HTTP/2 side, then what it recorded into the normal form."""
    connection = Http2Connection(respond)
    with contextlib.suppress(ParseError):
        for chunk in chunks:
            connection.receive(chunk)
    _exercise_forms(
        ConnectionRecord(1, _CLIENT_HELLO, None, None, "h2", tuple(connection.frames), tuple(connection.requests), None)
    )


def fuzz_http1(chunks: list[bytes]) -> None:
    """Target: the decrypted bytes of a connection into the HTTP/1.1 side, then what it recorded into the normal form."""
    connection = Http1Connection(respond)
    with contextlib.suppress(ParseError):
        for chunk in chunks:
            connection.receive(chunk)
    _exercise_forms(ConnectionRecord(1, _CLIENT_HELLO, None, None, "http/1.1", (), tuple(connection.requests), None))


def fuzz_request_head(head: bytes) -> None:
    """Target: a request head into the HTTP/1.1 parser."""
    with contextlib.suppress(ParseError):
        parse_request_head(head)


_INTERESTING_BYTES = (0x00, 0x01, 0x7F, 0x80, 0xFF)


def _mutate(seed: bytes, edits: list[tuple[int, int, int]]) -> bytes:
    """Apply edits, each (operation, position, value), to seed."""
    data = bytearray(seed)
    for operation, position, value in edits:
        if not data:
            break
        index = position % len(data)
        if operation == 0:
            data[index] = value
        elif operation == 1:
            data[index] = _INTERESTING_BYTES[value % len(_INTERESTING_BYTES)]
        elif operation == 2:
            del data[index:]
        elif operation == 3:
            del data[index : index + value % 16 + 1]
        else:
            data[index:index] = data[index : index + value % 64 + 1]
    return bytes(data)


def mutated(seeds: list[bytes]) -> SearchStrategy[bytes]:
    """Generate seeds with a few edits at random places."""
    edit = st.tuples(st.integers(0, 4), st.integers(0, 1 << 16), st.integers(0, 255))
    return st.builds(_mutate, st.sampled_from(seeds), st.lists(edit, max_size=8))


def in_chunks(streams: SearchStrategy[bytes]) -> SearchStrategy[list[bytes]]:
    """Generate streams cut into pieces, as reads from a socket would."""

    def cut(stream: bytes, sizes: list[int]) -> list[bytes]:
        chunks: list[bytes] = []
        position = 0
        for size in sizes:
            chunks.append(stream[position : position + size])
            position += size
        chunks.append(stream[position:])
        return chunks

    return st.builds(cut, streams, st.lists(st.integers(1, 256), max_size=6))


_REAL_CLIENT_HELLOS = [
    client_hello for stem in ("http1", "http2") for client_hello in samples.real_chrome(stem).client_hellos
]
_REAL_HTTP2_STREAMS = [stream for stream in samples.real_chrome("http2").client_streams if stream]
_REAL_HTTP1_STREAMS = [stream for stream in samples.real_chrome("http1").client_streams if stream]

_EXTENSION_TYPES = st.one_of(
    st.sampled_from(sorted(EXTENSIONS)), st.sampled_from([0x0A0A, 0xFAFA]), st.integers(0, 0xFFFF)
)


@st.composite
def _structured_client_hello(draw: st.DrawFn) -> bytes:
    """A ClientHello that is well-formed down to its extensions, whose content is random."""
    pieces = draw(st.lists(st.tuples(_EXTENSION_TYPES, st.binary(max_size=64)), max_size=8))
    suites = draw(st.lists(st.integers(0, 0xFFFF), max_size=8))
    extensions = [samples.extension(extension_type, data) for extension_type, data in pieces]
    message = samples.client_hello_message(extensions, cipher_suites=tuple(suites))
    return samples.records(message, record_size=draw(st.integers(1, 1 << 14)))


_FRAME_TYPES = st.one_of(st.integers(0, 0x10), st.sampled_from([0x0B, 0x2A, 0xE4]), st.integers(0, 0xFF))


@st.composite
def _structured_http2_stream(draw: st.DrawFn) -> bytes:
    """An HTTP/2 stream that is well-formed down to its frames, whose content is random or almost a real request."""
    frames: list[bytes] = []
    for _ in range(draw(st.integers(0, 8))):
        frame = draw(st.builds(Frame, _FRAME_TYPES, st.integers(0, 0xFF), st.integers(0, 7), st.binary(max_size=32)))
        frames.append(serialize_frame(frame.type, frame.flags, frame.stream_id, frame.payload))
        if draw(st.booleans()):
            target = draw(st.sampled_from([*KIND_PATHS.values(), "/other"]))
            frames.append(samples.http2_request_frame(target, stream_id=draw(st.integers(0, 3)) * 2 + 1))
    return PREFACE + b"".join(frames)


_HEADER_LINES = st.lists(
    st.tuples(
        st.sampled_from(["Host", "Content-Length", "Connection", "Transfer-Encoding", "X:Y", "", " a"]),
        st.binary(max_size=16),
    ),
    max_size=6,
).map(lambda pairs: b"".join(name.encode() + b": " + value + b"\r\n" for name, value in pairs))


@st.composite
def _structured_http1_stream(draw: st.DrawFn) -> bytes:
    """Requests with real targets and random headers."""
    requests: list[bytes] = []
    for _ in range(draw(st.integers(0, 3))):
        target = draw(st.sampled_from([*KIND_PATHS.values(), "/other"]))
        requests.append(f"GET {target} HTTP/1.1\r\n".encode() + draw(_HEADER_LINES) + b"\r\n")
    return b"".join(requests)


@given(in_chunks(st.one_of(st.binary(max_size=64), mutated(_REAL_CLIENT_HELLOS), _structured_client_hello())))
def test_fuzz_client_hello(chunks: list[bytes]) -> None:
    fuzz_client_hello(chunks)


@given(st.one_of(st.binary(max_size=64), mutated([hello[5:] for hello in _REAL_CLIENT_HELLOS])))
def test_fuzz_client_hello_message(message: bytes) -> None:
    fuzz_client_hello_message(message)


@given(in_chunks(st.one_of(st.binary(max_size=64), mutated(_REAL_HTTP2_STREAMS), _structured_http2_stream())))
def test_fuzz_http2(chunks: list[bytes]) -> None:
    fuzz_http2(chunks)


@given(in_chunks(st.one_of(st.binary(max_size=64), mutated(_REAL_HTTP1_STREAMS), _structured_http1_stream())))
def test_fuzz_http1(chunks: list[bytes]) -> None:
    fuzz_http1(chunks)


@given(st.one_of(st.binary(max_size=64), mutated([stream.partition(b"\r\n\r\n")[0] for stream in _REAL_HTTP1_STREAMS])))
def test_fuzz_request_head(head: bytes) -> None:
    fuzz_request_head(head)
