"""The public API: one blocking GET through a default Session of the native core."""

import functools
from dataclasses import dataclass, field

from mitto._native import RequestError, Session, encode_url

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


@functools.cache
def default_session() -> Session:
    """Return the Session that get() uses, which stays open until the process exits.

    Threads that race here may each create one. The ones that lose stay open unused, which does no harm.
    """
    return Session()


def get(url: str) -> Response:
    """Fetch `url` and block until the whole response has arrived.

    Raises:
        ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8.
        NativeLibraryError: If the native library cannot be used.
        RequestError: If the request ends without a response. An HTTP error status such as 404 is a response.
    """
    # An invalid URL is reported before anything else, whether or not there is a native library.
    encode_url(url)
    native = default_session().request(url)
    return Response(status=native.status, headers=native.headers, body=native.body)
