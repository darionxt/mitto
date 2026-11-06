"""Tests for the HTTP/2 side of the capture server."""

from collections.abc import Callable

import pytest
from hpack import Decoder, Encoder

from tests.capture import samples
from tools.capture.http2 import (
    ACK,
    CONTINUATION,
    DATA,
    END_HEADERS,
    END_STREAM,
    HEADERS,
    PADDED,
    PING,
    PREFACE,
    PRIORITY,
    PRIORITY_FLAG,
    RST_STREAM,
    SETTINGS,
    WINDOW_UPDATE,
    Frame,
    FrameParser,
    Http2Connection,
    Priority,
    parse_data,
    parse_headers,
    parse_priority,
    parse_settings,
    parse_window_update,
    serialize_frame,
)
from tools.capture.page import Response, respond
from tools.capture.wire import ParseError


def open_connection() -> Http2Connection:
    connection = Http2Connection(respond)
    connection.receive(PREFACE)
    return connection


def frames_of(output: bytes) -> list[Frame]:
    return FrameParser().feed(output)


def test_frame_parser_waits_for_whole_frames() -> None:
    wire = serialize_frame(WINDOW_UPDATE, 0, 3, bytes(4)) + serialize_frame(SETTINGS, ACK, 0)
    parser = FrameParser()

    assert parser.feed(wire[:12]) == []
    assert parser.feed(wire[12:]) == [
        Frame(WINDOW_UPDATE, 0, 3, bytes(4)),
        Frame(SETTINGS, ACK, 0, b""),
    ]


def test_frame_parser_ignores_the_reserved_bit() -> None:
    wire = b"\x00\x00\x00\x04\x00\x80\x00\x00\x05"

    assert FrameParser().feed(wire) == [Frame(SETTINGS, 0, 5, b"")]


def test_frame_parser_refuses_a_frame_over_the_limit() -> None:
    with pytest.raises(ParseError):
        FrameParser().feed(b"\x00\x40\x01\x00\x00\x00\x00\x00\x00")


def test_parses_settings_in_order() -> None:
    frame = Frame(SETTINGS, 0, 0, b"\x00\x04\x00\x60\x00\x00\x00\x01\x00\x01\x00\x00")

    assert parse_settings(frame) == ((4, 0x600000), (1, 0x10000))


def test_parses_a_window_update_without_its_reserved_bit() -> None:
    assert parse_window_update(Frame(WINDOW_UPDATE, 0, 0, b"\x80\x00\x00\x01")) == 1


def test_parses_priority_fields() -> None:
    frame = Frame(PRIORITY, 0, 3, b"\x80\x00\x00\x05\xff")

    assert parse_priority(frame) == Priority(exclusive=True, dependency=5, weight=256)


def test_parses_padded_headers_with_priority() -> None:
    frame = Frame(HEADERS, PADDED | PRIORITY_FLAG, 1, b"\x02\x00\x00\x00\x03\x00block\x00\x00")

    assert parse_headers(frame) == (Priority(exclusive=False, dependency=3, weight=1), b"block")


@pytest.mark.parametrize(
    ("parse", "frame"),
    [
        (parse_settings, Frame(SETTINGS, 0, 0, bytes(5))),
        (parse_window_update, Frame(WINDOW_UPDATE, 0, 0, bytes(3))),
        (parse_priority, Frame(PRIORITY, 0, 1, bytes(4))),
        (parse_headers, Frame(HEADERS, PADDED, 1, b"\x05abc")),
        (parse_headers, Frame(HEADERS, PADDED, 1, b"")),
        (parse_headers, Frame(HEADERS, PRIORITY_FLAG, 1, bytes(4))),
    ],
)
def test_refuses_malformed_payloads(parse: Callable[[Frame], object], frame: Frame) -> None:
    with pytest.raises(ParseError):
        parse(frame)


def test_starts_with_empty_settings() -> None:
    assert frames_of(Http2Connection(respond).start()) == [Frame(SETTINGS, 0, 0, b"")]


def test_refuses_a_bad_preface_at_the_first_wrong_byte() -> None:
    with pytest.raises(ParseError):
        Http2Connection(respond).receive(b"GET / HTTP/1.1\r\n")


def test_accepts_the_preface_in_pieces() -> None:
    connection = Http2Connection(respond)

    for position in range(len(PREFACE)):
        assert connection.receive(PREFACE[position : position + 1]) == b""
    output = connection.receive(serialize_frame(SETTINGS, 0, 0))

    assert frames_of(output) == [Frame(SETTINGS, ACK, 0, b"")]


def test_acknowledges_settings_only_once() -> None:
    connection = open_connection()

    assert connection.receive(serialize_frame(SETTINGS, ACK, 0)) == b""


def test_echoes_pings() -> None:
    connection = open_connection()

    assert frames_of(connection.receive(serialize_frame(PING, 0, 0, b"12345678"))) == [Frame(PING, ACK, 0, b"12345678")]
    assert connection.receive(serialize_frame(PING, ACK, 0, b"12345678")) == b""


def test_records_every_frame_in_order() -> None:
    connection = open_connection()
    connection.receive(
        serialize_frame(SETTINGS, 0, 0)
        + serialize_frame(WINDOW_UPDATE, 0, 0, b"\x00\x00\x00\x01")
        + serialize_frame(PRIORITY, 0, 3, bytes(5))
        + serialize_frame(0x2A, 0xFF, 1, b"grease")
        + samples.http2_request_frame("/fetch")
    )

    assert [frame.type for frame in connection.frames] == [SETTINGS, WINDOW_UPDATE, PRIORITY, 0x2A, HEADERS]


def test_answers_a_request() -> None:
    connection = open_connection()

    output = frames_of(connection.receive(samples.http2_request_frame("/fetch", stream_id=5)))

    assert [(frame.type, frame.stream_id) for frame in output] == [(HEADERS, 5), (DATA, 5)]
    assert output[0].flags == END_HEADERS
    assert output[1].flags == END_STREAM
    assert output[1].payload == b"fetched\n"
    decoded = dict(Decoder().decode(output[0].payload, raw=True))
    assert decoded[b":status"] == b"200"
    assert decoded[b"cache-control"] == b"no-store"
    assert decoded[b"content-length"] == b"8"


def test_answers_fit_a_client_that_allows_no_header_table() -> None:
    connection = open_connection()
    connection.receive(serialize_frame(SETTINGS, 0, 0, (1).to_bytes(2) + bytes(4)))
    decoder = Decoder()
    decoder.header_table_size = 0
    decoder.max_allowed_table_size = 0

    for stream_id in (1, 3):
        output = frames_of(connection.receive(samples.http2_request_frame("/fetch", stream_id=stream_id)))
        assert dict(decoder.decode(output[0].payload, raw=True))[b":status"] == b"200"


def test_keeps_header_order_and_flags() -> None:
    connection = open_connection()
    block = Encoder().encode([(":path", "/fetch"), ("b", "1"), ("a", "2"), ("b", "3")])
    payload = b"\x80\x00\x00\x00\xdb" + block

    connection.receive(serialize_frame(HEADERS, END_STREAM | END_HEADERS | PRIORITY_FLAG, 1, payload))

    request = connection.requests[0]
    assert request.stream_id == 1
    assert request.flags == END_STREAM | END_HEADERS | PRIORITY_FLAG
    assert request.priority == Priority(exclusive=True, dependency=0, weight=220)
    assert request.headers == ((":path", "/fetch"), ("b", "1"), ("a", "2"), ("b", "3"))


def test_decodes_with_one_table_per_connection() -> None:
    connection = open_connection()
    encoder = Encoder()

    connection.receive(samples.http2_request_frame("/fetch", stream_id=1, encoder=encoder))
    connection.receive(samples.http2_request_frame("/fetch", stream_id=3, encoder=encoder))

    assert connection.requests[0].headers == connection.requests[1].headers


def test_joins_continuation_frames() -> None:
    connection = open_connection()
    block = Encoder().encode([(":path", "/fetch"), ("name", "value")])

    assert connection.receive(serialize_frame(HEADERS, END_STREAM, 1, block[:3])) == b""
    assert connection.receive(serialize_frame(CONTINUATION, 0, 1, block[3:5])) == b""
    output = frames_of(connection.receive(serialize_frame(CONTINUATION, END_HEADERS, 1, block[5:])))

    assert [frame.type for frame in output] == [HEADERS, DATA]
    assert connection.requests[0].headers == ((":path", "/fetch"), ("name", "value"))
    assert connection.requests[0].flags == END_STREAM


def test_waits_for_the_end_of_a_request_body() -> None:
    connection = open_connection()
    block = Encoder().encode([(":method", "POST"), (":path", "/fetch")])

    assert connection.receive(serialize_frame(HEADERS, END_HEADERS, 1, block)) == b""
    assert connection.receive(serialize_frame(DATA, 0, 1, b"body")) == b""
    assert frames_of(connection.receive(serialize_frame(DATA, END_STREAM, 1)))[0].type == HEADERS


def test_trailers_are_not_a_new_request() -> None:
    connection = open_connection()
    encoder = Encoder()

    connection.receive(serialize_frame(HEADERS, END_HEADERS, 1, encoder.encode([(":path", "/fetch")])))
    output = connection.receive(serialize_frame(HEADERS, END_HEADERS | END_STREAM, 1, encoder.encode([("x", "y")])))

    assert len(connection.requests) == 1
    assert frames_of(output)[1].payload == b"fetched\n"


def test_does_not_answer_a_reset_stream() -> None:
    connection = open_connection()
    block = Encoder().encode([(":path", "/fetch")])

    connection.receive(serialize_frame(HEADERS, END_HEADERS, 1, block))
    connection.receive(serialize_frame(RST_STREAM, 0, 1, bytes(4)))

    assert connection.receive(serialize_frame(DATA, END_STREAM, 1)) == b""


def test_a_response_can_say_more_and_can_have_no_body() -> None:
    answer = Response(304, "text/plain", b"", "no-cache", (("ETag", '"a"'),))
    connection = Http2Connection(lambda _: answer)
    connection.receive(PREFACE)

    output = frames_of(connection.receive(samples.http2_request_frame("/")))

    assert [(frame.type, frame.flags) for frame in output] == [(HEADERS, END_HEADERS | END_STREAM)]
    headers = dict(Decoder().decode(output[0].payload, raw=True))
    assert headers[b":status"] == b"304"
    assert headers[b"cache-control"] == b"no-cache"
    assert headers[b"etag"] == b'"a"'


def test_parses_the_body_out_of_a_padded_data_frame() -> None:
    frame = Frame(DATA, PADDED, 1, b"\x02body\x00\x00")

    assert parse_data(frame) == b"body"
    assert parse_data(Frame(DATA, 0, 1, b"body")) == b"body"


@pytest.mark.parametrize("payload", [b"", b"\x05a"])
def test_refuses_padding_that_does_not_fit(payload: bytes) -> None:
    with pytest.raises(ParseError):
        parse_data(Frame(DATA, PADDED, 1, payload))


def test_splits_a_long_body_into_frames() -> None:
    connection = Http2Connection(lambda _: Response(200, "text/plain", bytes((1 << 14) + 1)))
    connection.receive(PREFACE)

    output = frames_of(connection.receive(samples.http2_request_frame("/")))

    assert [(frame.type, frame.flags, len(frame.payload)) for frame in output[1:]] == [
        (DATA, 0, 1 << 14),
        (DATA, END_STREAM, 1),
    ]


@pytest.mark.parametrize(
    ("description", "stream"),
    [
        ("frame inside a header block", serialize_frame(HEADERS, 0, 1) + serialize_frame(PING, 0, 0, bytes(8))),
        ("CONTINUATION with no header block", serialize_frame(CONTINUATION, END_HEADERS, 1)),
        ("CONTINUATION on another stream", serialize_frame(HEADERS, 0, 1) + serialize_frame(CONTINUATION, 0, 3)),
        ("HEADERS on the connection's stream", serialize_frame(HEADERS, END_HEADERS, 0)),
        ("HEADERS on a server's stream", serialize_frame(HEADERS, END_HEADERS, 2)),
        ("bad header block", serialize_frame(HEADERS, END_HEADERS, 1, b"\xff\xff\xff\xff\xff\xff")),
        ("bad SETTINGS", serialize_frame(SETTINGS, 0, 0, bytes(5))),
        ("bad WINDOW_UPDATE", serialize_frame(WINDOW_UPDATE, 0, 0, bytes(3))),
        ("bad PRIORITY", serialize_frame(PRIORITY, 0, 1, bytes(4))),
        ("bad RST_STREAM", serialize_frame(RST_STREAM, 0, 1, bytes(3))),
        ("bad PING", serialize_frame(PING, 0, 0, bytes(7))),
    ],
)
def test_refuses_protocol_errors(description: str, stream: bytes) -> None:
    connection = open_connection()

    with pytest.raises(ParseError):
        connection.receive(stream)


def test_refuses_an_endless_header_block() -> None:
    connection = open_connection()
    connection.receive(serialize_frame(HEADERS, 0, 1))
    fragment = serialize_frame(CONTINUATION, 0, 1, bytes(1 << 14))
    connection.receive(fragment * 64)

    with pytest.raises(ParseError):
        connection.receive(fragment)
