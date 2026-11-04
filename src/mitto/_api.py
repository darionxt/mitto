"""The public API: one blocking GET on top of the native core's C API."""

import ctypes
from dataclasses import dataclass, field

from mitto._native import MittoResponse, load_library

__all__ = ["RequestError", "Response", "get"]


@dataclass(frozen=True, slots=True)
class Response:
    """A finished response.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        headers: The final response's (name, value) pairs in the order received, so repeated headers stay separate.
            Headers have no fixed encoding, so they are decoded as Latin-1, which keeps every byte:
            value.encode("latin-1") gives back what the server sent.
        body: The final response's body, after any content decoding. It stays out of repr(), since it can be large.
    """

    status: int
    headers: tuple[tuple[str, str], ...]
    body: bytes = field(repr=False)


class RequestError(Exception):
    """A request ended without a response.

    Attributes:
        url: The URL that was requested.
        net_error: The negative Chromium net error code, such as -105 for ERR_NAME_NOT_RESOLVED.
    """

    def __init__(self, url: str, net_error: int) -> None:
        # Exception pickles its args, so they have to be what __init__ takes.
        super().__init__(url, net_error)
        self.url = url
        self.net_error = net_error

    def __str__(self) -> str:
        return f"GET {self.url} failed with net error {self.net_error}"


def get(url: str) -> Response:
    """Fetch `url` and block until the whole response has arrived.

    Raises:
        ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8.
        NativeLibraryError: If the native library cannot be used.
        RequestError: If the request ends without a response. An HTTP error status such as 404 is a response.
    """
    encoded_url = url.encode()
    # The C API takes a NUL-terminated string, so it would fetch only what comes before the NUL.
    if b"\0" in encoded_url:
        raise ValueError("url holds a NUL character")

    library = load_library()
    native = MittoResponse()
    net_error: int = library.mitto_get(encoded_url, ctypes.byref(native))
    if net_error != 0:
        raise RequestError(url, net_error)
    try:
        return Response(
            status=native.status,
            headers=_parse_headers(ctypes.string_at(native.headers, native.headers_size)),
            body=ctypes.string_at(native.body, native.body_size),
        )
    finally:
        library.mitto_response_free(ctypes.byref(native))


def _parse_headers(block: bytes) -> tuple[tuple[str, str], ...]:
    """Split the C API's "name: value\\r\\n" lines into pairs.

    Only CRLF ends a line. str.splitlines() would also split on characters that a value may hold, such as 0x85.
    """
    lines = block.decode("latin-1").split("\r\n")
    return tuple((name, value) for name, _, value in (line.partition(": ") for line in lines if line))
