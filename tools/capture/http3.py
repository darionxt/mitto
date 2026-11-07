"""A server side of HTTP/3 that records every stream and frame a client sends and needs no QUIC connection of its own.

QPACK, the header compression, comes from pylsqpack. Everything else about HTTP/3 is parsed here, because a recording
has to keep what an ordinary HTTP/3 stack hides: frame order, GREASE, and settings as they were sent.
"""

from collections import deque
from dataclasses import dataclass
from typing import NamedTuple

from pylsqpack import Decoder, DecompressionFailed, Encoder, EncoderStreamError, StreamBlocked

from tools.capture.page import Responder, Response
from tools.capture.wire import ParseError, Reader

DATA = 0x0
HEADERS = 0x1
SETTINGS = 0x4
GOAWAY = 0x7
PRIORITY_UPDATE_REQUEST = 0xF0700

CONTROL_STREAM = 0x0
QPACK_ENCODER_STREAM = 0x2
QPACK_DECODER_STREAM = 0x3

SETTINGS_QPACK_MAX_TABLE_CAPACITY = 0x1
SETTINGS_QPACK_BLOCKED_STREAMS = 0x7

QPACK_MAX_TABLE_CAPACITY = 4096
QPACK_BLOCKED_STREAMS = 16
"""What the server allows a client's QPACK encoder. Responses use no dynamic table."""

_SERVER_CONTROL_STREAM = 3
_SERVER_ENCODER_STREAM = 7
_SERVER_DECODER_STREAM = 11
_MAX_FRAME_SIZE = 1 << 20


@dataclass(frozen=True, slots=True)
class Http3Frame:
    """A frame as received. Only its type and length are interpreted."""

    stream_id: int
    type: int
    payload: bytes


@dataclass(frozen=True, slots=True)
class Http3Stream:
    """All a client sent on one stream.

    Attributes:
        id: The QUIC stream ID.
        type: The type a unidirectional stream announced, if any. A bidirectional stream holds a request and has none.
        data: The bytes that followed the type.
    """

    id: int
    type: int | None
    data: bytes


@dataclass(frozen=True, slots=True)
class Http3Request:
    """A request: the first header section on its stream.

    Attributes:
        stream_id: The stream the request came on.
        headers: The (name, value) pairs in order, decoded as Latin-1, which keeps every byte.
    """

    stream_id: int
    headers: tuple[tuple[str, str], ...]

    @property
    def target(self) -> str:
        """The :path pseudo-header, or nothing if the request has none."""
        return next((value for name, value in self.headers if name == ":path"), "")


class StreamData(NamedTuple):
    """Bytes to send on a stream, and whether they end it."""

    stream_id: int
    data: bytes
    end_stream: bool = False


def serialize_varint(value: int) -> bytes:
    """Return a QUIC variable-length integer in the fewest bytes it fits."""
    for bits, size in ((0, 1), (1, 2), (2, 4), (3, 8)):
        if value < 1 << (8 * size - 2):
            return ((bits << (8 * size - 2)) | value).to_bytes(size)
    raise ValueError(f"{value} does not fit a variable-length integer")


def serialize_frame(frame_type: int, payload: bytes = b"") -> bytes:
    """Return a frame in its wire form."""
    return serialize_varint(frame_type) + serialize_varint(len(payload)) + payload


def parse_settings(frame: Http3Frame) -> tuple[tuple[int, int], ...]:
    """Return a SETTINGS frame's (identifier, value) pairs, in order.

    Raises:
        ParseError: The payload is malformed.
    """
    reader = Reader(frame.payload)
    settings: list[tuple[int, int]] = []
    while not reader.empty:
        identifier = reader.varint()
        settings.append((identifier, reader.varint()))
    return tuple(settings)


def parse_priority_update(frame: Http3Frame) -> tuple[int, str]:
    """Return which request stream a PRIORITY_UPDATE frame is about, and the priority it gives that stream.

    Raises:
        ParseError: The payload is malformed.
    """
    reader = Reader(frame.payload)
    return reader.varint(), reader.rest().decode("latin-1")


def _peek_varint(buffer: bytearray, position: int) -> tuple[int, int] | None:
    """Return a variable-length integer at position and the position after it, or None if it is not all there."""
    if position >= len(buffer):
        return None
    end = position + (1 << (buffer[position] >> 6))
    if end > len(buffer):
        return None
    return int.from_bytes(bytes((buffer[position] & 0x3F,)) + bytes(buffer[position + 1 : end])), end


class FrameParser:
    """Splits the bytes of a stream into (type, payload) pairs."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        """Add bytes from the stream and return the frames they complete.

        Raises:
            ParseError: A frame is over the size this server allows.
        """
        self._buffer += data
        frames: list[tuple[int, bytes]] = []
        offset = 0
        while True:
            frame_type = _peek_varint(self._buffer, offset)
            size = None if frame_type is None else _peek_varint(self._buffer, frame_type[1])
            if frame_type is None or size is None:
                break
            if size[0] > _MAX_FRAME_SIZE:
                raise ParseError(f"frame of {size[0]} bytes")
            end = size[1] + size[0]
            if end > len(self._buffer):
                break
            frames.append((frame_type[0], bytes(self._buffer[size[1] : end])))
            offset = end
        del self._buffer[:offset]
        return frames


class _RequestStream:
    def __init__(self) -> None:
        self.parser = FrameParser()
        self.data = bytearray()
        # The header sections that have arrived but not been decoded. The first may be waiting for QPACK encoder data.
        self.header_sections: deque[bytes] = deque()
        self.blocked = False
        self.request: Http3Request | None = None
        self.finished = False
        self.reset = False
        self.answered = False


class _UnidirectionalStream:
    def __init__(self) -> None:
        self.type: int | None = None
        self.data = bytearray()
        self.parser = FrameParser()


class Http3Connection:
    """One HTTP/3 connection: takes the bytes a client sends on each stream and returns the bytes to send back.

    Attributes:
        frames: Every frame so far on the control and request streams, in the order they arrived.
        requests: Every request so far, in order.
    """

    def __init__(self, respond: Responder) -> None:
        self._respond = respond
        self._decoder = Decoder(QPACK_MAX_TABLE_CAPACITY, QPACK_BLOCKED_STREAMS)
        # Never told of a table it may use, the encoder uses none, so responses need no encoder stream data.
        self._encoder = Encoder()
        self._request_streams: dict[int, _RequestStream] = {}
        self._unidirectional_streams: dict[int, _UnidirectionalStream] = {}
        self.frames: list[Http3Frame] = []
        self.requests: list[Http3Request] = []

    @property
    def streams(self) -> tuple[Http3Stream, ...]:
        """What the client has sent on each stream, in the order the streams started."""
        streams = [Http3Stream(key, None, bytes(stream.data)) for key, stream in self._request_streams.items()]
        streams += [
            Http3Stream(key, stream.type, bytes(stream.data)) for key, stream in self._unidirectional_streams.items()
        ]
        return tuple(sorted(streams, key=lambda stream: stream.id))

    def start(self) -> list[StreamData]:
        """Return what the server sends first: the streams it must open, and its SETTINGS."""
        settings = b"".join(
            serialize_varint(identifier) + serialize_varint(value)
            for identifier, value in (
                (SETTINGS_QPACK_MAX_TABLE_CAPACITY, QPACK_MAX_TABLE_CAPACITY),
                (SETTINGS_QPACK_BLOCKED_STREAMS, QPACK_BLOCKED_STREAMS),
            )
        )
        return [
            StreamData(_SERVER_CONTROL_STREAM, serialize_varint(CONTROL_STREAM) + serialize_frame(SETTINGS, settings)),
            StreamData(_SERVER_ENCODER_STREAM, serialize_varint(QPACK_ENCODER_STREAM)),
            StreamData(_SERVER_DECODER_STREAM, serialize_varint(QPACK_DECODER_STREAM)),
        ]

    def receive(self, stream_id: int, data: bytes, end_stream: bool = False) -> list[StreamData]:
        """Take bytes from the client on a stream and return what to send back.

        Raises:
            ParseError: The client broke the protocol. The connection is unusable afterwards.
        """
        if stream_id % 4 == 0:
            return self._receive_request(stream_id, data, end_stream)
        if stream_id % 4 == 2:
            return self._receive_unidirectional(stream_id, data)
        raise ParseError(f"data on stream {stream_id}, which a client cannot open")

    def reset(self, stream_id: int) -> None:
        """Note that the client abandoned a stream, so it gets no answer. What it sent stays on record."""
        stream = self._request_streams.get(stream_id)
        if stream is not None:
            stream.reset = True

    def _receive_request(self, stream_id: int, data: bytes, end_stream: bool) -> list[StreamData]:
        stream = self._request_streams.setdefault(stream_id, _RequestStream())
        stream.data += data
        stream.finished = stream.finished or end_stream
        for frame_type, payload in stream.parser.feed(data):
            self.frames.append(Http3Frame(stream_id, frame_type, payload))
            if frame_type == HEADERS:
                stream.header_sections.append(payload)
        return self._advance_request(stream_id, stream)

    def _advance_request(self, stream_id: int, stream: _RequestStream) -> list[StreamData]:
        """Decode what the stream's header sections allow, and answer the request if the client has finished it."""
        output: list[StreamData] = []
        while stream.header_sections and not stream.blocked:
            try:
                control, headers = self._decoder.feed_header(stream_id, stream.header_sections[0])
            except StreamBlocked:
                stream.blocked = True
                break
            except DecompressionFailed as error:
                raise ParseError(f"bad header section: {error}") from error
            stream.header_sections.popleft()
            output += self._finish_header_section(stream_id, stream, control, headers)
        return output + self._answer(stream_id, stream)

    def _finish_header_section(
        self, stream_id: int, stream: _RequestStream, control: bytes, headers: list[tuple[bytes, bytes]]
    ) -> list[StreamData]:
        # A later header section on a stream holds trailers, not a new request.
        if stream.request is None:
            decoded = tuple((name.decode("latin-1"), value.decode("latin-1")) for name, value in headers)
            stream.request = Http3Request(stream_id, decoded)
            self.requests.append(stream.request)
        return [StreamData(_SERVER_DECODER_STREAM, control)] if control else []

    def _answer(self, stream_id: int, stream: _RequestStream) -> list[StreamData]:
        if stream.request is None or not stream.finished or stream.reset or stream.answered:
            return []
        stream.answered = True
        response = self._respond(stream.request)
        output = self._serialize(stream_id, response)
        if response.close:
            # What the client may still send on this connection, as the number of the first stream the server will not
            # take. Ending it here could overtake the answer, and even the handshake, if this came as early data.
            output.append(StreamData(_SERVER_CONTROL_STREAM, serialize_frame(GOAWAY, serialize_varint(stream_id + 4))))
        return output

    def _receive_unidirectional(self, stream_id: int, data: bytes) -> list[StreamData]:
        stream = self._unidirectional_streams.setdefault(stream_id, _UnidirectionalStream())
        stream.data += data
        if stream.type is None:
            found = _peek_varint(stream.data, 0)
            if found is None:
                return []
            stream.type = found[0]
            del stream.data[: found[1]]
            data = bytes(stream.data)
        if stream.type == CONTROL_STREAM:
            for frame_type, payload in stream.parser.feed(data):
                frame = Http3Frame(stream_id, frame_type, payload)
                self.frames.append(frame)
                if frame_type == SETTINGS:
                    parse_settings(frame)
        elif stream.type == QPACK_ENCODER_STREAM:
            return self._receive_encoder_data(data)
        return []

    def _receive_encoder_data(self, data: bytes) -> list[StreamData]:
        try:
            unblocked = self._decoder.feed_encoder(data)
        except EncoderStreamError as error:
            raise ParseError(f"bad QPACK encoder stream: {error}") from error
        output: list[StreamData] = []
        for stream_id in unblocked:
            stream = self._request_streams[stream_id]
            try:
                control, headers = self._decoder.resume_header(stream_id)
            except StreamBlocked:
                continue
            except DecompressionFailed as error:
                raise ParseError(f"bad header section: {error}") from error
            stream.blocked = False
            stream.header_sections.popleft()
            output += self._finish_header_section(stream_id, stream, control, headers)
            output += self._advance_request(stream_id, stream)
        return output

    def _serialize(self, stream_id: int, response: Response) -> list[StreamData]:
        _, section = self._encoder.encode(
            stream_id,
            [
                (b":status", str(response.status).encode()),
                (b"content-type", response.content_type.encode()),
                (b"content-length", str(len(response.body)).encode()),
                (b"cache-control", response.cache_control.encode()),
                *((name.lower().encode(), value.encode()) for name, value in response.headers),
            ],
        )
        data = serialize_frame(HEADERS, section)
        if response.body:
            data += serialize_frame(DATA, response.body)
        return [StreamData(stream_id, data, True)]
