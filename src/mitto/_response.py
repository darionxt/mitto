"""A response, as the public API returns it: finished, or with a body that arrives as the caller reads it."""

import asyncio
import json
from collections.abc import AsyncIterable, AsyncIterator, Iterable, Iterator
from dataclasses import dataclass, field
from typing import Any

from mitto._errors import MittoError
from mitto._native import NativeResponse, RedirectFollowed, ResponseStarted

__all__ = [
    "AsyncResponseStream",
    "HTTPStatusError",
    "Headers",
    "Redirect",
    "Response",
    "ResponseStream",
    "join_chunks",
    "to_async_response_stream",
    "to_response",
    "to_response_stream",
]

_DEFAULT_CHARSET = "utf-8"

# The first HTTP status code that says the request failed.
_FIRST_ERROR_STATUS = 400

# How many bytes an event loop joins by itself, which takes it well under a millisecond. Above this a thread does it, since
# bytes.join() lets go of the GIL for that long a copy.
_MOST_BYTES_TO_JOIN_ON_A_LOOP = 4 * 1024 * 1024


@dataclass(frozen=True, slots=True)
class Headers:
    """A response's headers in the order received, with lookup that ignores the case of a name.

    Iterating gives each (name, value) pair, so a repeated header stays separate. Headers have no fixed encoding, so they
    are decoded as Latin-1, which keeps every byte: value.encode("latin-1") gives back what the server sent.

    Attributes:
        pairs: The (name, value) pairs in the order received.
    """

    pairs: tuple[tuple[str, str], ...] = ()

    def __iter__(self) -> Iterator[tuple[str, str]]:
        return iter(self.pairs)

    def __len__(self) -> int:
        return len(self.pairs)

    def __contains__(self, name: str) -> bool:
        return bool(self.get_all(name))

    def __getitem__(self, name: str) -> str:
        """Return the value of a header, as get() does.

        Raises:
            KeyError: If there is no header of this name.
        """
        value = self.get(name)
        if value is None:
            raise KeyError(name)
        return value

    def get(self, name: str, default: str | None = None) -> str | None:
        """Return the value of a header, or `default` if there is none.

        Repeated headers come back joined by ", ", as Headers.get() does in Fetch. That does not suit Set-Cookie, whose
        values can hold commas, so use get_all() for it.
        """
        values = self.get_all(name)
        return ", ".join(values) if values else default

    def get_all(self, name: str) -> list[str]:
        """Return each value of a header in the order received, which is an empty list if there is none."""
        wanted = name.lower()
        return [value for pair_name, value in self.pairs if pair_name.lower() == wanted]


@dataclass(frozen=True, slots=True)
class Redirect:
    """A redirect that a request followed on its way to the response.

    Attributes:
        status: The HTTP status code of the redirect.
        url: The URL that answered with the redirect.
        headers: The redirect's headers.
    """

    status: int
    url: str
    headers: Headers


@dataclass(frozen=True, slots=True)
class Response:
    """A finished response.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        url: The final URL, after redirects.
        protocol: The protocol the response arrived over, for example "http/1.1", "h2" or "h3".
        headers: The final response's headers.
        content: The final response's body, after any content decoding. It stays out of repr(), since it can be large.
        history: The redirects that led here, the first one first.
        from_cache: Whether the body comes from the Session's HTTP cache. That goes for a response the server only had to
            confirm with a 304 too, so True does not say that no request went out.
    """

    status: int
    url: str
    protocol: str
    headers: Headers
    content: bytes = field(repr=False)
    history: tuple[Redirect, ...] = ()
    from_cache: bool = False

    @property
    def text(self) -> str:
        """The body decoded with the charset in Content-Type, otherwise as UTF-8. What does not decode becomes U+FFFD."""
        content_types = self.headers.get_all("Content-Type")
        charset = _declared_charset(content_types[-1]) if content_types else None
        try:
            return self.content.decode(charset or _DEFAULT_CHARSET, errors="replace")
        except (LookupError, ValueError):
            # A server can name anything: a codec that does not exist, one that is not for text such as zlib, or one that
            # refuses to replace such as idna.
            return self.content.decode(_DEFAULT_CHARSET, errors="replace")

    def json(self) -> Any:
        """Return the body parsed as JSON, which says its own encoding, so Content-Type does not matter.

        Raises:
            ValueError: If the body is not JSON. json.JSONDecodeError and UnicodeDecodeError are both ValueErrors.
        """
        return json.loads(self.content)

    def raise_for_status(self) -> None:
        """Do nothing, unless the status says that the request failed.

        Raises:
            HTTPStatusError: If the status is 400 or above.
        """
        if self.status >= _FIRST_ERROR_STATUS:
            raise HTTPStatusError(self)


@dataclass(frozen=True, slots=True)
class ResponseStream:
    """A response whose body arrives as the caller reads it, from Session.stream().

    Nothing more of the body comes off the network until it is asked for, so a slow reader makes the server wait, and a body
    of any size takes no more memory than the part in hand. What has been read is gone, so the two ways of reading take
    turns on the same bytes. Once the with statement has ended, reading raises a RequestError.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        url: The final URL, after redirects.
        protocol: The protocol the response arrived over, for example "http/1.1", "h2" or "h3".
        headers: The final response's headers.
        history: The redirects that led here, the first one first.
        from_cache: Whether the body comes from the Session's HTTP cache. That goes for a response the server only had to
            confirm with a 304 too, so True does not say that no request went out.
    """

    status: int
    url: str
    protocol: str
    headers: Headers
    history: tuple[Redirect, ...]
    from_cache: bool
    _chunks: Iterator[bytes] = field(repr=False)

    def iter_bytes(self) -> Iterator[bytes]:
        """Return an iterator over the rest of the body, after any content decoding, in parts as they arrive.

        Iterating raises a RequestError if the request ends before the end of the body, and that is the only way to learn
        that it did.
        """
        return self._chunks

    def read(self) -> bytes:
        """Return the rest of the body, after any content decoding, once all of it has arrived.

        Raises:
            RequestError: If the request ends before the end of the body.
        """
        return b"".join(self._chunks)


@dataclass(frozen=True, slots=True)
class AsyncResponseStream:
    """A response whose body arrives as the caller reads it, from AsyncSession.stream().

    This holds what ResponseStream holds, and reading follows the same rules, but for needing an await. A read that is
    cancelled, by a task or by asyncio.timeout(), cancels the request, so that a later read raises a RequestError and cannot
    pass for the end of the body. The `read` timer of a request is what waits for a slow server and leaves it running.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        url: The final URL, after redirects.
        protocol: The protocol the response arrived over, for example "http/1.1", "h2" or "h3".
        headers: The final response's headers.
        history: The redirects that led here, the first one first.
        from_cache: Whether the body comes from the Session's HTTP cache. That goes for a response the server only had to
            confirm with a 304 too, so True does not say that no request went out.
    """

    status: int
    url: str
    protocol: str
    headers: Headers
    history: tuple[Redirect, ...]
    from_cache: bool
    _chunks: AsyncIterator[bytes] = field(repr=False)

    def iter_bytes(self) -> AsyncIterator[bytes]:
        """Return an async iterator over the rest of the body, after any content decoding, in parts as they arrive.

        Iterating raises a RequestError if the request ends before the end of the body, and that is the only way to learn
        that it did.
        """
        return self._chunks

    async def read(self) -> bytes:
        """Return the rest of the body, after any content decoding, once all of it has arrived.

        Raises:
            RequestError: If the request ends before the end of the body.
        """
        return await join_chunks(self._chunks)


class HTTPStatusError(MittoError):
    """A response's status says that the request failed, as raised by Response.raise_for_status().

    Attributes:
        response: The response, whose status and URL are here too.
        status: The HTTP status code.
        url: The final URL, after redirects.
    """

    def __init__(self, response: Response) -> None:
        # Exception pickles its args, so they have to be what __init__ takes.
        super().__init__(response)
        self.response = response
        self.status = response.status
        self.url = response.url

    def __str__(self) -> str:
        return f"status {self.status} from {self.url}"


def to_response(native: NativeResponse) -> Response:
    """Return the public form of a response from the native core."""
    return Response(
        status=native.status,
        url=native.url,
        protocol=native.protocol,
        headers=Headers(native.headers),
        content=native.body,
        history=_to_redirects(native.history),
        from_cache=native.from_cache,
    )


def to_response_stream(
    started: ResponseStarted, history: Iterable[RedirectFollowed], chunks: Iterator[bytes]
) -> ResponseStream:
    """Return the public form of a response from the native core that has started, and whose body `chunks` gives."""
    return ResponseStream(
        status=started.status,
        url=started.url,
        protocol=started.protocol,
        headers=Headers(started.headers),
        history=_to_redirects(history),
        from_cache=started.from_cache,
        _chunks=chunks,
    )


def to_async_response_stream(
    started: ResponseStarted, history: Iterable[RedirectFollowed], chunks: AsyncIterator[bytes]
) -> AsyncResponseStream:
    """Return the public form of a response from the native core that has started, and whose body `chunks` gives."""
    return AsyncResponseStream(
        status=started.status,
        url=started.url,
        protocol=started.protocol,
        headers=Headers(started.headers),
        history=_to_redirects(history),
        from_cache=started.from_cache,
        _chunks=chunks,
    )


async def join_chunks(chunks: AsyncIterable[bytes]) -> bytes:
    """Return all that `chunks` gives as one value, without holding up the event loop for a large one."""
    parts = [chunk async for chunk in chunks]
    if sum(map(len, parts)) <= _MOST_BYTES_TO_JOIN_ON_A_LOOP:
        return b"".join(parts)
    return await asyncio.to_thread(b"".join, parts)


def _to_redirects(history: Iterable[RedirectFollowed]) -> tuple[Redirect, ...]:
    return tuple(Redirect(hop.status, hop.url, Headers(hop.headers)) for hop in history)


def _declared_charset(content_type: str) -> str | None:
    """Return the charset parameter of a Content-Type value, or None if it has none."""
    for parameter in content_type.split(";")[1:]:
        name, _, value = parameter.partition("=")
        if name.strip().lower() == "charset":
            return value.strip().strip('"')
    return None
