"""Parser for the TLS ClientHello, the first thing a client sends on a TLS connection."""

from dataclasses import dataclass

from tools.capture.wire import ParseError, Reader

_HANDSHAKE_RECORD = 22
_CLIENT_HELLO_MESSAGE = 1
_RECORD_HEADER_SIZE = 5
_MESSAGE_HEADER_SIZE = 4
_MAX_RECORD_SIZE = 1 << 14
_MAX_MESSAGE_SIZE = 1 << 16


@dataclass(frozen=True, slots=True)
class Extension:
    type: int
    data: bytes


@dataclass(frozen=True, slots=True)
class ClientHello:
    legacy_version: int
    random: bytes
    session_id: bytes
    cipher_suites: tuple[int, ...]
    compression_methods: tuple[int, ...]
    extensions: tuple[Extension, ...]


@dataclass(frozen=True, slots=True)
class ClientHelloRecords:
    """A ClientHello as it came off a TCP stream.

    Attributes:
        record_version: The version field of the first TLS record, which is not the version the client offers.
        raw: The TLS records that carried the message, unchanged.
        hello: The parsed message.
    """

    record_version: int
    raw: bytes
    hello: ClientHello


def parse_client_hello(message: bytes) -> ClientHello:
    """Parse a complete ClientHello handshake message, including its type and length.

    Raises:
        ParseError: The message is malformed.
    """
    reader = Reader(message)
    if reader.uint(1) != _CLIENT_HELLO_MESSAGE:
        raise ParseError("not a ClientHello message")
    body = reader.vector(3)
    reader.finish()

    legacy_version = body.uint(2)
    random = body.take(32)
    session_id = body.opaque(1)
    cipher_suites = body.vector(2).uints(2)
    compression_methods = body.vector(1).uints(1)
    extensions: list[Extension] = []
    # The extensions block as a whole is optional.
    if not body.empty:
        block = body.vector(2)
        while not block.empty:
            extension_type = block.uint(2)
            extensions.append(Extension(extension_type, block.opaque(2)))
    body.finish()
    return ClientHello(legacy_version, random, session_id, cipher_suites, compression_methods, tuple(extensions))


class ClientHelloAssembler:
    """Collects TLS records from a TCP stream until they hold a whole ClientHello.

    A ClientHello can span TCP segments and, in principle, TLS records, so a single read rarely settles it.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()
        self._consumed: int = 0
        self._message = bytearray()
        self._record_version: int | None = None

    def feed(self, data: bytes) -> ClientHelloRecords | None:
        """Add bytes from the stream. Returns the ClientHello once it is complete, and None until then.

        Raises:
            ParseError: The stream does not start with a valid ClientHello.
        """
        self._buffer += data
        while len(self._buffer) - self._consumed >= _RECORD_HEADER_SIZE:
            header = Reader(bytes(self._buffer[self._consumed : self._consumed + _RECORD_HEADER_SIZE]))
            record_type = header.uint(1)
            version = header.uint(2)
            size = header.uint(2)
            if record_type != _HANDSHAKE_RECORD:
                raise ParseError(f"record type {record_type} where a TLS handshake record should be")
            if size == 0 or size > _MAX_RECORD_SIZE:
                raise ParseError(f"TLS record of {size} bytes")
            end = self._consumed + _RECORD_HEADER_SIZE + size
            if end > len(self._buffer):
                return None
            if self._record_version is None:
                self._record_version = version
            self._message += self._buffer[self._consumed + _RECORD_HEADER_SIZE : end]
            self._consumed = end
            if len(self._message) < _MESSAGE_HEADER_SIZE:
                continue
            message_size = _MESSAGE_HEADER_SIZE + int.from_bytes(self._message[1:_MESSAGE_HEADER_SIZE])
            if message_size > _MAX_MESSAGE_SIZE:
                raise ParseError(f"ClientHello of {message_size} bytes")
            if len(self._message) >= message_size:
                hello = parse_client_hello(bytes(self._message[:message_size]))
                return ClientHelloRecords(self._record_version, bytes(self._buffer[: self._consumed]), hello)
        return None
