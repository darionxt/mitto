"""A server side of HTTP/1.1 that records each request's head and needs no socket of its own."""

from dataclasses import dataclass, replace

from tools.capture.page import Responder, Response
from tools.capture.wire import ParseError

_MAX_HEAD_SIZE = 1 << 18
_MAX_BODY_SIZE = 1 << 24
_REASONS = {200: "OK", 302: "Found", 304: "Not Modified", 404: "Not Found"}


@dataclass(frozen=True, slots=True)
class Http1Request:
    """A request head. Text is decoded as Latin-1, which keeps every byte.

    Attributes:
        method: The method, as sent.
        target: The request target, as sent.
        version: The HTTP version, as sent, such as HTTP/1.1.
        headers: The (name, value) pairs with their original case and order.
        body: The body that came with a Content-Length, once all of it has arrived.
    """

    method: str
    target: str
    version: str
    headers: tuple[tuple[str, str], ...]
    body: bytes = b""


def parse_request_head(head: bytes) -> Http1Request:
    """Parse a request line and its header lines, without the blank line that ends them.

    Raises:
        ParseError: The head is malformed.
    """
    request_line, *header_lines = head.decode("latin-1").split("\r\n")
    if any("\r" in line or "\n" in line for line in (request_line, *header_lines)):
        raise ParseError("a line break that is not CRLF")
    parts = request_line.split(" ")
    if len(parts) != 3 or not all(parts) or not parts[2].startswith("HTTP/1."):
        raise ParseError(f"bad request line {request_line!r}")
    headers: list[tuple[str, str]] = []
    for line in header_lines:
        name, separator, value = line.partition(":")
        if not separator or not name or any(character in name for character in " \t"):
            raise ParseError(f"bad header line {line!r}")
        headers.append((name, value.strip(" \t")))
    return Http1Request(parts[0], parts[1], parts[2], tuple(headers))


class Http1Connection:
    """One HTTP/1.1 connection: takes the bytes a client sends and returns the bytes to send back.

    Attributes:
        requests: Every request so far, in order.
        closed: Whether the server should close the connection after sending what it has returned so far.
    """

    def __init__(self, respond: Responder) -> None:
        self._respond = respond
        self._buffer = bytearray()
        # The request whose body is on its way, and the size of that body.
        self._waiting: Http1Request | None = None
        self._body_size = 0
        self.requests: list[Http1Request] = []
        self.closed = False

    def start(self) -> bytes:
        """Return what the server sends first, before any request: nothing, in HTTP/1.1."""
        return b""

    def receive(self, data: bytes) -> bytes:
        """Take bytes from the client and return what to send back.

        Raises:
            ParseError: A request is malformed. The connection is unusable afterwards.
        """
        self._buffer += data
        output = bytearray()
        while not self.closed:
            if self._waiting is None and not self._read_head():
                break
            if len(self._buffer) < self._body_size:
                break
            output += self._answer()
        return bytes(output)

    def _read_head(self) -> bool:
        """Take a request head from the buffer, if all of it is there."""
        end = self._buffer.find(b"\r\n\r\n")
        if end < 0:
            if len(self._buffer) > _MAX_HEAD_SIZE:
                raise ParseError(f"request head over {_MAX_HEAD_SIZE} bytes")
            return False
        request = parse_request_head(bytes(self._buffer[:end]))
        del self._buffer[: end + 4]
        self._body_size = _body_size(request)
        self._waiting = request
        # It goes on record now, so that a client that never sends the body still shows.
        self.requests.append(request)
        return True

    def _answer(self) -> bytes:
        request = self._waiting
        assert request is not None
        if self._body_size:
            request = replace(request, body=bytes(self._buffer[: self._body_size]))
            self.requests[-1] = request
            del self._buffer[: self._body_size]
        self._waiting = None
        self._body_size = 0
        headers = {name.lower(): value for name, value in request.headers}
        # A chunked body has no length to wait for. Nothing here needs one, so the connection ends with this answer.
        response = self._respond(request)
        self.closed = (
            response.close or "transfer-encoding" in headers or headers.get("connection", "").lower() == "close"
        )
        return _serialize(response, close=self.closed)


def _body_size(request: Http1Request) -> int:
    length = next((value for name, value in request.headers if name.lower() == "content-length"), "0")
    if not (length.isascii() and length.isdigit()):
        raise ParseError(f"bad Content-Length {length!r}")
    size = int(length)
    if size > _MAX_BODY_SIZE:
        raise ParseError(f"request body of {size} bytes")
    return size


def _serialize(response: Response, *, close: bool) -> bytes:
    head = (
        f"HTTP/1.1 {response.status} {_REASONS[response.status]}\r\n"
        f"Content-Type: {response.content_type}\r\n"
        f"Content-Length: {len(response.body)}\r\n"
        f"Cache-Control: {response.cache_control}\r\n"
        f"{''.join(f'{name}: {value}\r\n' for name, value in response.headers)}"
        f"Connection: {'close' if close else 'keep-alive'}\r\n"
        "\r\n"
    )
    return head.encode("latin-1") + response.body
