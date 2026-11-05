"""A server side of HTTP/2 that records every frame a client sends and needs no socket of its own."""

from dataclasses import dataclass

from hpack import Decoder, Encoder, HPACKError

from tools.capture.page import Responder, Response
from tools.capture.wire import ParseError, Reader

PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

DATA = 0x0
HEADERS = 0x1
PRIORITY = 0x2
RST_STREAM = 0x3
SETTINGS = 0x4
PING = 0x6
WINDOW_UPDATE = 0x8
CONTINUATION = 0x9

END_STREAM = 0x1
ACK = 0x1
END_HEADERS = 0x4
PADDED = 0x8
PRIORITY_FLAG = 0x20

_FRAME_HEADER_SIZE = 9
_MAX_FRAME_SIZE = 1 << 14
_MAX_HEADER_BLOCK_SIZE = 1 << 20
_MAX_HEADER_LIST_SIZE = 1 << 18
_PING_SIZE = 8
_PRIORITY_SIZE = 5
_RST_STREAM_SIZE = 4
_WINDOW_UPDATE_SIZE = 4
_SETTING_SIZE = 6
_STREAM_ID_MASK = 0x7FFFFFFF


@dataclass(frozen=True, slots=True)
class Frame:
    """A frame as received. Only its fixed header is interpreted."""

    type: int
    flags: int
    stream_id: int
    payload: bytes


@dataclass(frozen=True, slots=True)
class Priority:
    """The fields of a PRIORITY frame, and of a HEADERS frame with the PRIORITY flag."""

    exclusive: bool
    dependency: int
    weight: int


@dataclass(frozen=True, slots=True)
class Http2Request:
    """A request: what its first HEADERS frame said, and the header block that followed.

    Attributes:
        stream_id: The stream the request came on.
        flags: The flags of the HEADERS frame. If CONTINUATION frames follow it, END_HEADERS is on the last of them.
        priority: The priority fields of the HEADERS frame, if it has any.
        headers: The (name, value) pairs in order, decoded as Latin-1, which keeps every byte.
    """

    stream_id: int
    flags: int
    priority: Priority | None
    headers: tuple[tuple[str, str], ...]

    @property
    def target(self) -> str:
        """The :path pseudo-header, or nothing if the request has none."""
        return next((value for name, value in self.headers if name == ":path"), "")


class FrameParser:
    """Splits a stream of bytes, after the connection preface, into frames."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[Frame]:
        """Add bytes from the stream and return the frames they complete.

        Raises:
            ParseError: A frame is over the size this server allows, which is the protocol's default limit.
        """
        self._buffer += data
        frames: list[Frame] = []
        offset = 0
        while len(self._buffer) - offset >= _FRAME_HEADER_SIZE:
            header = Reader(bytes(self._buffer[offset : offset + _FRAME_HEADER_SIZE]))
            size = header.uint(3)
            frame_type = header.uint(1)
            flags = header.uint(1)
            stream_id = header.uint(4) & _STREAM_ID_MASK
            if size > _MAX_FRAME_SIZE:
                raise ParseError(f"frame of {size} bytes")
            end = offset + _FRAME_HEADER_SIZE + size
            if end > len(self._buffer):
                break
            frames.append(Frame(frame_type, flags, stream_id, bytes(self._buffer[offset + _FRAME_HEADER_SIZE : end])))
            offset = end
        del self._buffer[:offset]
        return frames


def parse_settings(frame: Frame) -> tuple[tuple[int, int], ...]:
    """Return a SETTINGS frame's (identifier, value) pairs, in order.

    Raises:
        ParseError: The payload is not a whole number of settings.
    """
    if len(frame.payload) % _SETTING_SIZE:
        raise ParseError(f"SETTINGS payload of {len(frame.payload)} bytes")
    reader = Reader(frame.payload)
    settings: list[tuple[int, int]] = []
    while not reader.empty:
        identifier = reader.uint(2)
        settings.append((identifier, reader.uint(4)))
    return tuple(settings)


def parse_window_update(frame: Frame) -> int:
    """Return a WINDOW_UPDATE frame's increment.

    Raises:
        ParseError: The payload has the wrong size.
    """
    if len(frame.payload) != _WINDOW_UPDATE_SIZE:
        raise ParseError(f"WINDOW_UPDATE payload of {len(frame.payload)} bytes")
    return int.from_bytes(frame.payload) & _STREAM_ID_MASK


def parse_priority(frame: Frame) -> Priority:
    """Return a PRIORITY frame's fields.

    Raises:
        ParseError: The payload has the wrong size.
    """
    if len(frame.payload) != _PRIORITY_SIZE:
        raise ParseError(f"PRIORITY payload of {len(frame.payload)} bytes")
    return _read_priority(Reader(frame.payload))


def parse_headers(frame: Frame) -> tuple[Priority | None, bytes]:
    """Return a HEADERS frame's priority fields, if it has them, and its fragment of the header block.

    Raises:
        ParseError: The padding or the priority fields do not fit the payload.
    """
    reader = Reader(frame.payload)
    padding = reader.uint(1) if frame.flags & PADDED else 0
    priority = _read_priority(reader) if frame.flags & PRIORITY_FLAG else None
    rest = reader.rest()
    if padding > len(rest):
        raise ParseError(f"{padding} bytes of padding in {len(rest)} bytes")
    return priority, rest[: len(rest) - padding]


def _read_priority(reader: Reader) -> Priority:
    dependency = reader.uint(4)
    weight = reader.uint(1) + 1
    return Priority(bool(dependency >> 31), dependency & _STREAM_ID_MASK, weight)


def serialize_frame(frame_type: int, flags: int, stream_id: int, payload: bytes = b"") -> bytes:
    """Return a frame in its wire form."""
    header = len(payload).to_bytes(3) + bytes((frame_type, flags)) + stream_id.to_bytes(4)
    return header + payload


class Http2Connection:
    """One HTTP/2 connection: takes the bytes a client sends and returns the bytes to send back.

    Attributes:
        frames: Every frame so far, in order, including those of the requests.
        requests: Every request so far, in order.
        closed: Whether the server should close the connection. HTTP/2 here never asks for that.
    """

    def __init__(self, respond: Responder) -> None:
        self._respond = respond
        self._preface = bytearray()
        self._parser = FrameParser()
        self._decoder = Decoder(max_header_list_size=_MAX_HEADER_LIST_SIZE)
        self._encoder = Encoder()
        # With no table, responses stay within any limit a client's SETTINGS can set, whatever it sets.
        self._encoder.header_table_size = 0
        # The header block being received: its first frame, and the fragments so far.
        self._block_start: Frame | None = None
        self._block = bytearray()
        self._seen_streams: set[int] = set()
        self._unanswered: dict[int, str] = {}
        self.frames: list[Frame] = []
        self.requests: list[Http2Request] = []
        self.closed = False

    def start(self) -> bytes:
        """Return what the server sends first: its SETTINGS, which leave everything at the default."""
        return serialize_frame(SETTINGS, 0, 0)

    def receive(self, data: bytes) -> bytes:
        """Take bytes from the client and return what to send back.

        Raises:
            ParseError: The client broke the protocol. The connection is unusable afterwards.
        """
        if len(self._preface) < len(PREFACE):
            taken = data[: len(PREFACE) - len(self._preface)]
            self._preface += taken
            data = data[len(taken) :]
            if not PREFACE.startswith(self._preface):
                raise ParseError("bad HTTP/2 connection preface")
        output = bytearray()
        for frame in self._parser.feed(data):
            self.frames.append(frame)
            output += self._handle(frame)
        return bytes(output)

    def _handle(self, frame: Frame) -> bytes:
        if self._block_start is not None and frame.type != CONTINUATION:
            raise ParseError(f"frame type {frame.type} inside a header block")
        if frame.type == HEADERS:
            return self._start_block(frame)
        if frame.type == CONTINUATION:
            return self._continue_block(frame)
        if frame.type == DATA:
            return self._finish_stream(frame.stream_id) if frame.flags & END_STREAM else b""
        if frame.type == SETTINGS:
            parse_settings(frame)
            return b"" if frame.flags & ACK else serialize_frame(SETTINGS, ACK, 0)
        if frame.type == PING:
            if len(frame.payload) != _PING_SIZE:
                raise ParseError(f"PING payload of {len(frame.payload)} bytes")
            return b"" if frame.flags & ACK else serialize_frame(PING, ACK, 0, frame.payload)
        if frame.type == PRIORITY:
            parse_priority(frame)
        elif frame.type == WINDOW_UPDATE:
            parse_window_update(frame)
        elif frame.type == RST_STREAM:
            if len(frame.payload) != _RST_STREAM_SIZE:
                raise ParseError(f"RST_STREAM payload of {len(frame.payload)} bytes")
            self._unanswered.pop(frame.stream_id, None)
        return b""

    def _start_block(self, frame: Frame) -> bytes:
        if frame.stream_id % 2 == 0:
            raise ParseError(f"HEADERS on stream {frame.stream_id}, which a client cannot open")
        _, fragment = parse_headers(frame)
        self._block_start = frame
        self._block = bytearray(fragment)
        return self._finish_block() if frame.flags & END_HEADERS else b""

    def _continue_block(self, frame: Frame) -> bytes:
        if self._block_start is None or frame.stream_id != self._block_start.stream_id:
            raise ParseError("CONTINUATION outside a header block")
        self._block += frame.payload
        if len(self._block) > _MAX_HEADER_BLOCK_SIZE:
            raise ParseError(f"header block over {_MAX_HEADER_BLOCK_SIZE} bytes")
        return self._finish_block() if frame.flags & END_HEADERS else b""

    def _finish_block(self) -> bytes:
        start = self._block_start
        assert start is not None
        self._block_start = None
        try:
            headers = self._decoder.decode(bytes(self._block), raw=True)
        except HPACKError as error:
            raise ParseError(f"bad header block: {error}") from error
        stream_id = start.stream_id
        # A second header block on a stream holds trailers, not a new request.
        if stream_id not in self._seen_streams:
            priority, _ = parse_headers(start)
            decoded = tuple((name.decode("latin-1"), value.decode("latin-1")) for name, value in headers)
            request = Http2Request(stream_id, start.flags, priority, decoded)
            self.requests.append(request)
            self._seen_streams.add(stream_id)
            self._unanswered[stream_id] = request.target
        return self._finish_stream(stream_id) if start.flags & END_STREAM else b""

    def _finish_stream(self, stream_id: int) -> bytes:
        """Answer the request on a stream the client has finished sending on, unless it has an answer already."""
        target = self._unanswered.pop(stream_id, None)
        return b"" if target is None else self._serialize(stream_id, self._respond(target))

    def _serialize(self, stream_id: int, response: Response) -> bytes:
        block = self._encoder.encode(
            [
                (":status", str(response.status)),
                ("content-type", response.content_type),
                ("content-length", str(len(response.body))),
                ("cache-control", "no-store"),
            ]
        )
        output = bytearray(serialize_frame(HEADERS, END_HEADERS, stream_id, block))
        body = response.body
        # Flow control is not enforced: these responses are far smaller than the windows real clients open.
        while len(body) > _MAX_FRAME_SIZE:
            output += serialize_frame(DATA, 0, stream_id, body[:_MAX_FRAME_SIZE])
            body = body[_MAX_FRAME_SIZE:]
        output += serialize_frame(DATA, END_STREAM, stream_id, body)
        return bytes(output)
