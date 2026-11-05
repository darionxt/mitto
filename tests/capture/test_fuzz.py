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
from pylsqpack import Encoder as PylsqpackEncoder

from tests.capture import samples
from tools.capture import http3
from tools.capture.certificate import generate_certificate
from tools.capture.client_hello import ClientHelloAssembler, ClientHelloRecords, parse_client_hello
from tools.capture.http1 import Http1Connection, parse_request_head
from tools.capture.http2 import PREFACE, Frame, Http2Connection, serialize_frame
from tools.capture.http3 import Http3Connection, serialize_varint
from tools.capture.names import EXTENSIONS, TRANSPORT_PARAMETERS
from tools.capture.normal_form import RecordingError, normal_client_hello, normal_form, normal_hello
from tools.capture.page import KIND_PATHS, respond
from tools.capture.quic import (
    InitialAssembler,
    QuicClientHello,
    parse_initial_frames,
    parse_transport_parameters,
)
from tools.capture.quic_server import LiveQuicConnection, QuicDispatcher
from tools.capture.recording import ConnectionRecord, QuicConnectionRecord, Recording, raw_form
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


def fuzz_quic_initial(datagrams: list[bytes]) -> None:
    """Target: datagrams into the Initial packet parser, then what it found into the normal form."""
    assembler = InitialAssembler()
    for datagram in datagrams:
        with contextlib.suppress(ParseError):
            found = assembler.feed(datagram)
            if found is not None:
                json.dumps(normal_hello(found.hello))
                return


def fuzz_initial_frames(payload: bytes) -> None:
    """Target: a decrypted packet into the frame parser."""
    with contextlib.suppress(ParseError):
        parse_initial_frames(payload)


def fuzz_transport_parameters(data: bytes) -> None:
    """Target: a quic_transport_parameters extension into its parser and into the normal form, which must not fail."""
    with contextlib.suppress(ParseError):
        parse_transport_parameters(data)
    message = samples.client_hello_message([samples.extension(_QUIC_TRANSPORT_PARAMETERS, data)])
    json.dumps(normal_hello(parse_client_hello(message)))


def fuzz_http3(writes: list[tuple[int, bytes, bool]]) -> None:
    """Target: the bytes a client sends on each stream into the HTTP/3 side, then what it recorded into both forms."""
    connection = Http3Connection(respond)
    with contextlib.suppress(ParseError):
        for stream_id, data, end_stream in writes:
            connection.receive(stream_id, data, end_stream)
    _exercise_forms(
        QuicConnectionRecord(
            1,
            _QUIC_CLIENT_HELLO,
            6,
            None,
            None,
            "h3",
            connection.streams,
            tuple(connection.frames),
            tuple(connection.requests),
            None,
        )
    )


def fuzz_quic_dispatcher(datagrams: list[bytes]) -> None:
    """Target: datagrams into the QUIC side of the server, which drops what makes no sense and must never fail."""
    recorded: list[LiveQuicConnection] = []
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, recorded.append)
    now = 1.0
    for datagram in datagrams:
        dispatcher.receive(datagram, ("127.0.0.1", 1234), now)
        now += 1.0
        dispatcher.handle_timers(now)
    assert dispatcher.next_wakeup() is None or isinstance(dispatcher.next_wakeup(), float)
    for connection in recorded:
        _exercise_forms(connection.snapshot())


def _exercise_forms(connection: ConnectionRecord | QuicConnectionRecord) -> None:
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
_QUIC_TRANSPORT_PARAMETERS = 57
_CERTIFICATE = generate_certificate(samples.HOST)


def _a_quic_client_hello() -> QuicClientHello:
    found = InitialAssembler().feed(samples.initial_packet(samples.crypto_frame(0, samples.client_hello_message())))
    assert found is not None
    return found


_QUIC_CLIENT_HELLO = _a_quic_client_hello()


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


_REAL_CHROME_HTTP3 = samples.real_chrome_http3()
_REAL_DATAGRAMS = [datagram for datagrams in _REAL_CHROME_HTTP3.datagrams for datagram in datagrams]
_REAL_CLIENT_HELLO_MESSAGES = [
    connection.client_hello.message
    for connection in _REAL_CHROME_HTTP3.recording.connections
    if isinstance(connection, QuicConnectionRecord)
]
_REAL_TRANSPORT_PARAMETERS = [
    extension.data
    for connection in _REAL_CHROME_HTTP3.recording.connections
    if isinstance(connection, QuicConnectionRecord)
    for extension in connection.client_hello.hello.extensions
    if extension.type == _QUIC_TRANSPORT_PARAMETERS
]

_INITIAL_FRAME_TYPES = st.sampled_from([0x00, 0x01, 0x02, 0x03, 0x06, 0x1C])


@st.composite
def _structured_initial_frames(draw: st.DrawFn) -> bytes:
    """Frames of the types an Initial packet can hold, with random content."""
    frames: list[bytes] = []
    for _ in range(draw(st.integers(0, 8))):
        frame_type = draw(_INITIAL_FRAME_TYPES)
        if frame_type == 0x06:
            offset = draw(st.integers(0, 1 << 18))
            frames.append(samples.crypto_frame(offset, draw(st.binary(max_size=64))))
        else:
            frames.append(serialize_varint(frame_type) + draw(st.binary(max_size=8)))
    return b"".join(frames)


@st.composite
def _encrypted_initial_datagrams(draw: st.DrawFn) -> list[bytes]:
    """Datagrams that decrypt, so that what they hold reaches the frame parser and the ClientHello parser."""
    message = draw(st.one_of(mutated(_REAL_CLIENT_HELLO_MESSAGES), st.binary(max_size=64)))
    frames = [samples.crypto_frame(0, message), draw(_structured_initial_frames())]
    if draw(st.booleans()):
        frames.reverse()
    datagrams = [
        samples.initial_packet(
            frame,
            packet_number=number,
            destination=draw(st.sampled_from([samples.CONNECTION_ID, b"", bytes(20)])),
            keys_from=samples.CONNECTION_ID,
            source=draw(st.binary(max_size=20)),
            token=draw(st.binary(max_size=8)),
        )
        for number, frame in enumerate(frames)
    ]
    # Bytes of padding, another packet in the same datagram, or a damaged datagram.
    if draw(st.booleans()):
        datagrams[-1] += draw(st.sampled_from([bytes(32), b"\x40" + bytes(8), datagrams[0]]))
    return datagrams


def _stream_ids() -> SearchStrategy[int]:
    return st.one_of(st.sampled_from([0, 2, 4, 6, 8, 10]), st.sampled_from([1, 3]), st.integers(0, 64))


@st.composite
def _structured_http3_writes(draw: st.DrawFn) -> list[tuple[int, bytes, bool]]:
    """Streams that are well-formed down to their frames, whose content is random or a real header section."""
    writes: list[tuple[int, bytes, bool]] = []
    writes.append(
        (
            2,
            serialize_varint(http3.CONTROL_STREAM)
            + http3.serialize_frame(http3.SETTINGS, draw(st.binary(max_size=16))),
            False,
        )
    )
    writes.append((6, serialize_varint(http3.QPACK_ENCODER_STREAM) + draw(st.binary(max_size=16)), False))
    for _ in range(draw(st.integers(0, 6))):
        stream_id = draw(st.sampled_from([0, 2, 4, 6, 8]))
        frame_type = draw(st.sampled_from([0x0, 0x1, 0x4, 0x7, 0xD, 0x21, 0xF0700]))
        target = draw(st.sampled_from([*KIND_PATHS.values(), "/other"]))
        section = PylsqpackEncoder().encode(stream_id, [(b":path", target.encode()), (b"a", b"b")])[1]
        payload = draw(st.one_of(st.binary(max_size=16), st.just(section)))
        writes.append((stream_id, http3.serialize_frame(frame_type, payload), draw(st.booleans())))
    return writes


_REAL_HTTP3_WRITES = [
    [(stream_id, data, stream_id % 4 == 0) for stream_id, data in streams] for streams in _REAL_CHROME_HTTP3.streams
]


def _mutated_http3_writes() -> SearchStrategy[list[tuple[int, bytes, bool]]]:
    def mutate(
        writes: list[tuple[int, bytes, bool]], edits: list[tuple[int, int, int]], which: int
    ) -> list[tuple[int, bytes, bool]]:
        index = which % len(writes)
        stream_id, data, end_stream = writes[index]
        changed = list(writes)
        changed[index] = (stream_id, _mutate(data, edits), end_stream)
        return changed

    edit = st.tuples(st.integers(0, 4), st.integers(0, 1 << 16), st.integers(0, 255))
    return st.builds(mutate, st.sampled_from(_REAL_HTTP3_WRITES), st.lists(edit, max_size=8), st.integers(0, 64))


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


@given(
    st.one_of(
        st.lists(st.binary(max_size=64), max_size=3),
        st.lists(mutated(_REAL_DATAGRAMS), max_size=3),
        _encrypted_initial_datagrams(),
    )
)
def test_fuzz_quic_initial(datagrams: list[bytes]) -> None:
    fuzz_quic_initial(datagrams)


@given(st.one_of(st.binary(max_size=64), _structured_initial_frames()))
def test_fuzz_initial_frames(payload: bytes) -> None:
    fuzz_initial_frames(payload)


_TRANSPORT_PARAMETER_IDS = st.one_of(st.sampled_from(sorted(TRANSPORT_PARAMETERS)), st.integers(0, (1 << 62) - 1))


@given(
    st.one_of(
        st.binary(max_size=64),
        mutated(_REAL_TRANSPORT_PARAMETERS),
        st.lists(st.tuples(_TRANSPORT_PARAMETER_IDS, st.binary(max_size=16)), max_size=8).map(
            lambda pairs: b"".join(samples.transport_parameter(identifier, value) for identifier, value in pairs)
        ),
    )
)
def test_fuzz_transport_parameters(data: bytes) -> None:
    fuzz_transport_parameters(data)


@given(
    st.one_of(
        st.lists(st.tuples(_stream_ids(), st.binary(max_size=32), st.booleans()), max_size=8),
        _mutated_http3_writes(),
        _structured_http3_writes(),
    )
)
def test_fuzz_http3(writes: list[tuple[int, bytes, bool]]) -> None:
    fuzz_http3(writes)


@given(
    st.one_of(
        st.lists(st.binary(max_size=64), max_size=4),
        st.lists(mutated(_REAL_DATAGRAMS), max_size=4),
        st.lists(st.sampled_from(_REAL_DATAGRAMS), max_size=4),
        _encrypted_initial_datagrams().map(lambda datagrams: [datagram.ljust(1200, b"\x00") for datagram in datagrams]),
    )
)
def test_fuzz_quic_dispatcher(datagrams: list[bytes]) -> None:
    fuzz_quic_dispatcher(datagrams)
