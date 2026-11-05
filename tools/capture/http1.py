"""A server side of HTTP/1.1 that records each request's head and needs no socket of its own."""

from dataclasses import dataclass

from tools.capture.page import Responder, Response
from tools.capture.wire import ParseError

_MAX_HEAD_SIZE = 1 << 18
_REASONS = {200: "OK", 404: "Not Found"}


@dataclass(frozen=True, slots=True)
class Http1Request:
    """A request head. Text is decoded as Latin-1, which keeps every byte.

    Attributes:
        method: The method, as sent.
        target: The request target, as sent.
        version: The HTTP version, as sent, such as HTTP/1.1.
        headers: The (name, value) pairs with their original case and order.
    """

    method: str
    target: str
    version: str
    headers: tuple[tuple[str, str], ...]


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
        self._body_left = 0
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
            skipped = min(self._body_left, len(self._buffer))
            del self._buffer[:skipped]
            self._body_left -= skipped
            if self._body_left:
                break
            end = self._buffer.find(b"\r\n\r\n")
            if end < 0:
                if len(self._buffer) > _MAX_HEAD_SIZE:
                    raise ParseError(f"request head over {_MAX_HEAD_SIZE} bytes")
                break
            request = parse_request_head(bytes(self._buffer[:end]))
            del self._buffer[: end + 4]
            self.requests.append(request)
            output += self._answer(request)
        return bytes(output)

    def _answer(self, request: Http1Request) -> bytes:
        headers = {name.lower(): value for name, value in request.headers}
        length = headers.get("content-length", "0")
        if not (length.isascii() and length.isdigit()):
            raise ParseError(f"bad Content-Length {length!r}")
        self._body_left = int(length)
        # A chunked body has no length to skip. Nothing here needs one, so the connection ends with this answer.
        self.closed = "transfer-encoding" in headers or headers.get("connection", "").lower() == "close"
        return _serialize(self._respond(request.target), close=self.closed)


def _serialize(response: Response, *, close: bool) -> bytes:
    head = (
        f"HTTP/1.1 {response.status} {_REASONS[response.status]}\r\n"
        f"Content-Type: {response.content_type}\r\n"
        f"Content-Length: {len(response.body)}\r\n"
        "Cache-Control: no-store\r\n"
        f"Connection: {'close' if close else 'keep-alive'}\r\n"
        "\r\n"
    )
    return head.encode("latin-1") + response.body
