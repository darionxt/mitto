"""Tests for the HTTP/3 side of the capture server."""

import pytest
from pylsqpack import Decoder, Encoder

from tools.capture.http3 import (
    CONTROL_STREAM,
    DATA,
    HEADERS,
    PRIORITY_UPDATE_REQUEST,
    QPACK_BLOCKED_STREAMS,
    QPACK_DECODER_STREAM,
    QPACK_ENCODER_STREAM,
    QPACK_MAX_TABLE_CAPACITY,
    SETTINGS,
    FrameParser,
    Http3Connection,
    Http3Frame,
    Http3Request,
    Http3Stream,
    StreamData,
    parse_priority_update,
    parse_settings,
    serialize_frame,
    serialize_varint,
)
from tools.capture.page import Response, respond
from tools.capture.wire import ParseError

CONTROL = 2
ENCODER = 6
FIRST = 0
SECOND = 4
_HEADERS = [(b":method", b"GET"), (b":authority", b"capture.localhost"), (b":scheme", b"https"), (b":path", b"/fetch")]
_GREASE_FRAME = 0x21 + 0x1F * 3
_GREASE_STREAM = 0x21 + 0x1F * 5


def header_section(headers: list[tuple[bytes, bytes]] = _HEADERS, stream_id: int = FIRST) -> bytes:
    """Return a header section that refers to no dynamic table."""
    return Encoder().encode(stream_id, headers)[1]


def request_frame(headers: list[tuple[bytes, bytes]] = _HEADERS, stream_id: int = FIRST) -> bytes:
    return serialize_frame(HEADERS, header_section(headers, stream_id))


def dynamic_section() -> tuple[bytes, bytes, bytes]:
    """Return a header section that refers to the dynamic table, with the encoder stream data it needs, in two parts."""
    encoder = Encoder()
    capacity = encoder.apply_settings(QPACK_MAX_TABLE_CAPACITY, QPACK_BLOCKED_STREAMS)
    # The encoder puts a header in its table only when it has seen it before.
    encoder.encode(1000, _HEADERS)
    insertions, section = encoder.encode(FIRST, _HEADERS)
    assert insertions
    return bytes((QPACK_ENCODER_STREAM,)) + capacity, insertions, serialize_frame(HEADERS, section)


def decode_response(output: list[StreamData]) -> tuple[list[tuple[bytes, bytes]], bytes, bool]:
    """Return the headers and body of the one response in output, and whether it ended the stream."""
    assert len(output) == 1
    stream_id, data, end_stream = output[0]
    frames = FrameParser().feed(data)
    assert [frame_type for frame_type, _ in frames] in ([HEADERS], [HEADERS, DATA])
    _, headers = Decoder(0, 0).feed_header(stream_id, frames[0][1])
    return headers, frames[1][1] if len(frames) == 2 else b"", end_stream


def test_starts_with_the_streams_a_server_must_open() -> None:
    output = Http3Connection(respond).start()

    assert [(item.stream_id, item.end_stream) for item in output] == [(3, False), (7, False), (11, False)]
    assert output[0].data[0] == CONTROL_STREAM
    assert output[1].data == bytes((QPACK_ENCODER_STREAM,))
    assert output[2].data == bytes((QPACK_DECODER_STREAM,))
    frames = FrameParser().feed(output[0].data[1:])
    assert frames[0][0] == SETTINGS
    settings = parse_settings(Http3Frame(3, *frames[0]))
    assert settings == ((1, QPACK_MAX_TABLE_CAPACITY), (7, QPACK_BLOCKED_STREAMS))


def test_answers_a_request_when_the_client_finishes_it() -> None:
    connection = Http3Connection(respond)

    assert connection.receive(FIRST, request_frame(), end_stream=False) == []
    output = connection.receive(FIRST, b"", end_stream=True)

    headers, body, end_stream = decode_response(output)
    assert headers == [
        (b":status", b"200"),
        (b"content-type", b"text/plain; charset=utf-8"),
        (b"content-length", b"8"),
        (b"cache-control", b"no-store"),
    ]
    assert body == b"fetched\n"
    assert end_stream
    assert connection.requests == [
        Http3Request(FIRST, tuple((name.decode(), value.decode()) for name, value in _HEADERS))
    ]
    assert connection.requests[0].target == "/fetch"


def test_a_request_can_arrive_in_pieces() -> None:
    connection = Http3Connection(respond)
    frame = request_frame()

    for byte in frame[:-1]:
        assert connection.receive(FIRST, bytes((byte,))) == []
    output = connection.receive(FIRST, frame[-1:], end_stream=True)

    assert decode_response(output)[1] == b"fetched\n"
    assert connection.frames == [Http3Frame(FIRST, HEADERS, frame[2:])]


def test_a_response_can_say_more_and_can_have_no_body() -> None:
    answer = Response(304, "text/plain", b"", "no-cache", (("ETag", '"a"'),))
    connection = Http3Connection(lambda _: answer)

    output = connection.receive(FIRST, request_frame(), end_stream=True)

    assert [frame_type for frame_type, _ in FrameParser().feed(output[0].data)] == [HEADERS]
    headers, body, ended = decode_response(output)
    assert dict(headers)[b":status"] == b"304"
    assert dict(headers)[b"cache-control"] == b"no-cache"
    assert dict(headers)[b"etag"] == b'"a"'
    assert body == b""
    assert ended


def test_a_missing_path_is_not_found() -> None:
    connection = Http3Connection(lambda request: Response(404, "text/plain", request.target.encode()))

    output = connection.receive(FIRST, request_frame([(b":method", b"GET")]), end_stream=True)

    assert decode_response(output)[1] == b""
    assert connection.requests[0].target == ""


def test_keeps_header_names_order_and_values() -> None:
    headers = [(b"z", b"1"), (b"a", b"2"), (b"z", b"\xe9")]
    connection = Http3Connection(respond)

    connection.receive(FIRST, request_frame(headers), end_stream=True)

    assert connection.requests[0].headers == (("z", "1"), ("a", "2"), ("z", "\xe9"))


def test_trailers_are_not_a_new_request() -> None:
    connection = Http3Connection(respond)
    frames = request_frame() + serialize_frame(DATA, b"body") + request_frame([(b"trailer", b"1")])

    output = connection.receive(FIRST, frames, end_stream=True)

    assert len(connection.requests) == 1
    assert decode_response(output)[1] == b"fetched\n"
    assert [frame.type for frame in connection.frames] == [HEADERS, DATA, HEADERS]


def test_answers_only_once() -> None:
    connection = Http3Connection(respond)
    assert connection.receive(FIRST, request_frame(), end_stream=True)

    assert connection.receive(FIRST, b"", end_stream=True) == []


def test_a_stream_the_client_abandoned_gets_no_answer() -> None:
    connection = Http3Connection(respond)
    connection.receive(FIRST, request_frame())

    connection.reset(FIRST)

    assert connection.receive(FIRST, b"", end_stream=True) == []
    assert len(connection.requests) == 1


def test_resetting_an_unknown_stream_does_nothing() -> None:
    Http3Connection(respond).reset(CONTROL)


def test_a_frame_may_come_before_the_headers() -> None:
    connection = Http3Connection(respond)
    frames = serialize_frame(_GREASE_FRAME, b"xyz") + request_frame()

    output = connection.receive(FIRST, frames, end_stream=True)

    assert decode_response(output)[1] == b"fetched\n"
    assert [frame.type for frame in connection.frames] == [_GREASE_FRAME, HEADERS]


def test_records_control_frames_in_order() -> None:
    connection = Http3Connection(respond)
    settings = serialize_varint(1) + serialize_varint(65536)
    update = serialize_varint(FIRST) + b"u=0, i"
    stream = (
        serialize_varint(CONTROL_STREAM)
        + serialize_frame(SETTINGS, settings)
        + serialize_frame(_GREASE_FRAME, b"xyz")
        + serialize_frame(PRIORITY_UPDATE_REQUEST, update)
    )

    assert connection.receive(CONTROL, stream) == []

    assert connection.frames == [
        Http3Frame(CONTROL, SETTINGS, settings),
        Http3Frame(CONTROL, _GREASE_FRAME, b"xyz"),
        Http3Frame(CONTROL, PRIORITY_UPDATE_REQUEST, update),
    ]
    assert parse_settings(connection.frames[0]) == ((1, 65536),)
    assert parse_priority_update(connection.frames[2]) == (FIRST, "u=0, i")
    assert connection.streams == (Http3Stream(CONTROL, CONTROL_STREAM, stream[1:]),)


def test_a_stream_type_can_arrive_in_pieces() -> None:
    connection = Http3Connection(respond)
    long_type = serialize_varint(_GREASE_STREAM + 0x1F * 3)
    assert len(long_type) == 2

    connection.receive(10, long_type[:1])
    assert connection.streams == (Http3Stream(10, None, long_type[:1]),)
    connection.receive(10, long_type[1:] + b"abc")
    connection.receive(10, b"def")

    assert connection.streams == (Http3Stream(10, _GREASE_STREAM + 0x1F * 3, b"abcdef"),)
    assert connection.frames == []


def test_streams_are_listed_by_number() -> None:
    connection = Http3Connection(respond)
    connection.receive(10, bytes((QPACK_DECODER_STREAM,)) + b"\x01")
    connection.receive(SECOND, request_frame(stream_id=SECOND))
    connection.receive(CONTROL, bytes((CONTROL_STREAM,)))
    connection.receive(FIRST, b"")

    assert [(stream.id, stream.type) for stream in connection.streams] == [
        (FIRST, None),
        (CONTROL, CONTROL_STREAM),
        (SECOND, None),
        (10, QPACK_DECODER_STREAM),
    ]
    assert connection.streams[2].data == request_frame(stream_id=SECOND)


def test_decodes_headers_that_use_the_dynamic_table() -> None:
    capacity, insertions, frame = dynamic_section()
    connection = Http3Connection(respond)

    connection.receive(ENCODER, capacity + insertions)
    output = connection.receive(FIRST, frame, end_stream=True)

    assert connection.requests[0].target == "/fetch"
    # The section acknowledgement goes on the server's decoder stream, ahead of the answer.
    assert output[0].stream_id == 11
    assert output[1].stream_id == FIRST


def test_a_request_waits_for_the_encoder_stream_it_refers_to() -> None:
    capacity, insertions, frame = dynamic_section()
    connection = Http3Connection(respond)

    assert connection.receive(FIRST, frame, end_stream=True) == []
    assert connection.requests == []

    output = connection.receive(ENCODER, capacity + insertions)

    assert connection.requests[0].target == "/fetch"
    assert output[-1].stream_id == FIRST
    assert output[-1].end_stream


def test_trailers_wait_behind_a_blocked_request() -> None:
    capacity, insertions, frame = dynamic_section()
    connection = Http3Connection(respond)

    assert connection.receive(FIRST, frame + request_frame([(b"trailer", b"1")]), end_stream=True) == []
    output = connection.receive(ENCODER, capacity + insertions)

    assert [request.target for request in connection.requests] == ["/fetch"]
    assert output[-1].stream_id == FIRST


def test_a_blocked_request_stays_blocked_until_enough_has_arrived() -> None:
    capacity, insertions, frame = dynamic_section()
    connection = Http3Connection(respond)
    connection.receive(FIRST, frame, end_stream=True)

    assert connection.receive(ENCODER, capacity) == []
    assert connection.requests == []
    assert connection.receive(ENCODER, insertions)

    assert len(connection.requests) == 1


def test_an_abandoned_blocked_request_stays_on_record_without_an_answer() -> None:
    capacity, insertions, frame = dynamic_section()
    connection = Http3Connection(respond)
    connection.receive(FIRST, frame, end_stream=True)
    connection.reset(FIRST)

    output = connection.receive(ENCODER, capacity + insertions)

    assert all(item.stream_id != FIRST for item in output)
    assert len(connection.requests) == 1


def test_the_control_stream_is_not_a_request_even_with_headers_on_it() -> None:
    connection = Http3Connection(respond)

    assert connection.receive(CONTROL, bytes((CONTROL_STREAM,)) + request_frame()) == []

    assert connection.requests == []


@pytest.mark.parametrize("stream_id", [1, 3])
def test_refuses_a_stream_a_client_cannot_open(stream_id: int) -> None:
    with pytest.raises(ParseError, match="cannot open"):
        Http3Connection(respond).receive(stream_id, b"x")


def test_refuses_a_header_section_that_does_not_decode() -> None:
    with pytest.raises(ParseError, match="header section"):
        Http3Connection(respond).receive(FIRST, serialize_frame(HEADERS, b"\xff\xff\xff"))


def test_refuses_encoder_data_that_does_not_decode() -> None:
    with pytest.raises(ParseError, match="encoder stream"):
        Http3Connection(respond).receive(ENCODER, bytes((QPACK_ENCODER_STREAM,)) + b"\xff" * 16)


def test_refuses_settings_that_do_not_parse() -> None:
    stream = bytes((CONTROL_STREAM,)) + serialize_frame(SETTINGS, b"\x01")
    with pytest.raises(ParseError):
        Http3Connection(respond).receive(CONTROL, stream)


def test_refuses_a_frame_over_the_limit() -> None:
    with pytest.raises(ParseError, match="frame of"):
        FrameParser().feed(serialize_varint(DATA) + serialize_varint(1 << 21))


def test_the_frame_parser_waits_for_a_whole_frame() -> None:
    parser = FrameParser()
    frame = serialize_frame(0xF0700, b"payload")

    assert parser.feed(frame[:1]) == []
    assert parser.feed(frame[1:5]) == []
    assert parser.feed(frame[5:] + frame) == [(0xF0700, b"payload"), (0xF0700, b"payload")]


@pytest.mark.parametrize("value", [0, 63, 64, 16383, 16384, (1 << 30) - 1, 1 << 30, (1 << 62) - 1])
def test_varints_take_the_fewest_bytes(value: int) -> None:
    expected = 1 if value < 64 else 2 if value < 1 << 14 else 4 if value < 1 << 30 else 8

    assert len(serialize_varint(value)) == expected


def test_a_varint_has_a_limit() -> None:
    with pytest.raises(ValueError, match="does not fit"):
        serialize_varint(1 << 62)


def test_parse_priority_update_refuses_a_payload_without_a_stream() -> None:
    with pytest.raises(ParseError):
        parse_priority_update(Http3Frame(CONTROL, PRIORITY_UPDATE_REQUEST, b""))
