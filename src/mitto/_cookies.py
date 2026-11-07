"""A Session's cookies: what the cookie store in //net holds, and a file to keep them in.

//net decides which cookies a response sets and which a request sends. This module only reads and changes that store,
through the native core, so that nothing here can disagree with it. See docs/parity.md for how a Session uses them.
"""

import asyncio
import json
import os
import tempfile
from collections.abc import Iterator
from dataclasses import KW_ONLY, dataclass, field
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any, Literal

from mitto import _native

__all__ = ["AsyncCookies", "Cookie", "CookiePartitionKey", "Cookies", "write_cookie_file"]

type SameSite = Literal["unspecified", "none", "lax", "strict"]
"""A cookie's SameSite attribute. "unspecified" is a cookie without one, which //net treats as "lax"."""

type CookiePriority = Literal["low", "medium", "high"]
"""A cookie's Priority attribute, which says what the store evicts first."""

type CookieSourceScheme = Literal["unset", "non_secure", "secure"]
"""The kind of scheme that a cookie came from."""

type CookieSourceType = Literal["http", "script", "other"]
"""What a cookie came from: a response, a page's script or something else, such as a caller of this API."""

_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)
_MICROSECOND = timedelta(microseconds=1)


@dataclass(frozen=True, slots=True)
class CookiePartitionKey:
    """The partition of a cookie that a request sends only from one top-level site.

    Attributes:
        top_level_site: The site, such as "https://example.com".
        has_cross_site_ancestor: Whether a frame between it and the cookie's frame is cross-site.
    """

    top_level_site: str
    has_cross_site_ancestor: bool


@dataclass(frozen=True, slots=True)
class Cookie:
    """One cookie, as the cookie store in //net holds it.

    A cookie has no fixed encoding, so `name`, `value` and `path` hold one character for each byte, as a Response's
    headers do: value.encode("latin-1") gives back what the server sent. To set text in another encoding, encode it and
    decode that as Latin-1.

    Without `created`, this is a new cookie to set, and //net checks it as if a server at its domain had sent it. The
    attributes below `created` then stay None. A cookie from Cookies has them all, and can be set again as it is.

    Attributes:
        name: The name.
        value: The value. It stays out of repr(), since it can be a secret.
        domain: Where it goes. With a leading dot it is a domain cookie, which also goes to subdomains. Without one it is
            a host-only cookie, which goes to that host alone.
        path: The path that it goes to, and below.
        secure: Whether it goes only over HTTPS and other secure schemes.
        http_only: Whether a page's script cannot see it. A Session has no scripts yet, so nothing differs.
        same_site: Its SameSite attribute.
        priority: Its Priority attribute.
        expires: When it ends, as a timezone-aware time, or None for a session cookie. //net brings a time more than 400
            days after `created` down to that, as it does for a server's.
        partition_key: Its partition, or None if it is not partitioned.
        created: When it was created, as a timezone-aware time, which also decides its place in a Cookie header.
        last_accessed: When a request last sent it.
        last_updated: When it last changed.
        source_scheme: The kind of scheme it came from.
        source_port: The port it came from, -1 for none, or -2 if that was not valid.
        source_type: What it came from.
    """

    name: str
    value: str = field(repr=False)
    _: KW_ONLY
    domain: str
    path: str = "/"
    secure: bool = False
    http_only: bool = False
    same_site: SameSite = "unspecified"
    priority: CookiePriority = "medium"
    expires: datetime | None = None
    partition_key: CookiePartitionKey | None = None
    created: datetime | None = None
    last_accessed: datetime | None = None
    last_updated: datetime | None = None
    source_scheme: CookieSourceScheme | None = None
    source_port: int | None = None
    source_type: CookieSourceType | None = None


class _CookieStore:
    """What the cookie store in //net holds, for each API's kind of cookies to read and change.

    Each call asks the network thread, and blocks only the calling thread for as long as that takes, with the GIL
    released. The calls may come from any thread, but not after the Session has closed, when they raise a ValueError.
    """

    def __init__(self, native: _native.Session) -> None:
        self._native = native

    def __iter__(self) -> Iterator[Cookie]:
        """Iterate over the cookies as they are now, with the longest path first, then the earliest created.

        An expired cookie is left out.
        """
        return iter(self._list())

    def __len__(self) -> int:
        return len(self._list())

    def get(self, name: str, *, domain: str | None = None, path: str | None = None) -> Cookie | None:
        """Return the cookie of this name, or None if there is none.

        Args:
            name: The name.
            domain: The domain, written as Cookie.domain is, or None for any.
            path: The path, or None for any.

        Returns:
            The first that matches, if several do, which is the one that goes first in a Cookie header.
        """
        for cookie in self._list():
            if cookie.name == name and domain in (None, cookie.domain) and path in (None, cookie.path):
                return cookie
        return None

    def set(self, cookie: Cookie) -> None:
        """Put a cookie in the store, as a response's Set-Cookie would. It replaces one of the same name, domain and path.

        A cookie with an expiry in the past is not stored, and what it would replace still goes. A dot before a public
        suffix gives a cookie for that one name, as a server's Domain attribute does, and not one for all of the suffix.

        Raises:
            ValueError: If the native core refuses it, such as a __Host- cookie that is not Secure or a domain that holds a
                port or a path.
            TypeError: If one of its times has no timezone.
        """
        self._native.set_cookies(
            json.dumps({"version": _native.COOKIES_VERSION, "cookies": [_to_native(cookie)]}).encode()
        )

    def delete(self, name: str, *, domain: str | None = None, path: str | None = None) -> int:
        """Delete the cookies of this name, and return how many went.

        Args:
            name: The name.
            domain: The domain, written as Cookie.domain is, or None for any.
            path: The path, or None for any.
        """
        keys = {"name": name, "domain": domain, "path": path}
        return self._delete({key: value for key, value in keys.items() if value is not None})

    def clear(self) -> int:
        """Delete every cookie, and return how many went."""
        return self._delete({})

    def _set_from_file(self, path: str | os.PathLike[str], contents: bytes) -> None:
        try:
            self._native.set_cookies(contents)
        except ValueError as error:
            raise ValueError(f"{path} does not hold cookies that this Session can use") from error

    def _list(self) -> list[Cookie]:
        return [_from_native(cookie) for cookie in json.loads(self._native.list_cookies())["cookies"]]

    def _delete(self, keys: dict[str, str]) -> int:
        return self._native.delete_cookies(json.dumps({"version": _native.COOKIES_VERSION, **keys}).encode())


class Cookies(_CookieStore):
    """The cookies of one Session.

    Each call asks the network thread, and blocks only the calling thread for as long as that takes, with the GIL
    released. The calls may come from any thread, but not after the Session has closed, when they raise a ValueError.
    """

    def save(self, path: str | os.PathLike[str]) -> None:
        """Write all the cookies to a JSON file that only its owner can read or write.

        The values are not encrypted. Session cookies are written too, as a browser does that restores its last session.
        A crash during this call leaves the file as it was.

        Raises:
            OSError: If the file cannot be written.
        """
        write_cookie_file(path, self._native.list_cookies())

    def load(self, path: str | os.PathLike[str]) -> None:
        """Add the cookies of a file from save(), each with the attributes it had there, replacing any of the same name.

        Raises:
            OSError: If the file cannot be read, such as a FileNotFoundError.
            ValueError: If the native core refuses what the file holds, in which case no cookie is added.
        """
        self._set_from_file(path, Path(path).read_bytes())


class AsyncCookies(_CookieStore):
    """The cookies of one AsyncSession, which are those of Cookies with the file left to a thread of the event loop's.

    What the network thread answers is one short task on it, which never waits for Python, so those calls stay plain ones,
    on the event loop's thread. They raise a ValueError once the AsyncSession has begun to close.
    """

    async def save(self, path: str | os.PathLike[str]) -> None:
        """Write all the cookies to a file off the event loop, as Cookies.save() does.

        Raises:
            OSError: If the file cannot be written.
        """
        # This asks the network thread here, so that nothing off the loop can use a native Session that is closing.
        await asyncio.to_thread(write_cookie_file, path, self._native.list_cookies())

    async def load(self, path: str | os.PathLike[str]) -> None:
        """Read a file from save() off the event loop and add its cookies, as Cookies.load() does.

        Raises:
            OSError: If the file cannot be read, such as a FileNotFoundError.
            ValueError: If the native core refuses what the file holds, in which case no cookie is added.
        """
        self._set_from_file(path, await asyncio.to_thread(Path(path).read_bytes))


def write_cookie_file(path: str | os.PathLike[str], contents: bytes) -> None:
    """Write what the native core lists as a file of cookies that only its owner can read or write.

    Raises:
        OSError: If the file cannot be written.
    """
    _replace_file(Path(path).resolve(), contents)


def _to_microseconds(time: datetime | None) -> int | None:
    return None if time is None else (time - _EPOCH) // _MICROSECOND


def _to_datetime(microseconds: int | None) -> datetime | None:
    return None if microseconds is None else _EPOCH + microseconds * _MICROSECOND


def _to_native(cookie: Cookie) -> dict[str, object]:
    """Return one cookie as the C API's mitto_session_set_cookies() takes it."""
    key = cookie.partition_key
    attributes = {
        "expires_us": _to_microseconds(cookie.expires),
        "partition_key": None
        if key is None
        else {"top_level_site": key.top_level_site, "has_cross_site_ancestor": key.has_cross_site_ancestor},
        "created_us": _to_microseconds(cookie.created),
        "last_accessed_us": _to_microseconds(cookie.last_accessed),
        "last_updated_us": _to_microseconds(cookie.last_updated),
        "source_scheme": cookie.source_scheme,
        "source_port": cookie.source_port,
        "source_type": cookie.source_type,
    }
    return {
        "name": cookie.name,
        "value": cookie.value,
        "domain": cookie.domain,
        "path": cookie.path,
        "secure": cookie.secure,
        "http_only": cookie.http_only,
        "same_site": cookie.same_site,
        "priority": cookie.priority,
        **{name: value for name, value in attributes.items() if value is not None},
    }


def _from_native(native: dict[str, Any]) -> Cookie:
    """Return one cookie as the C API's mitto_session_list_cookies() writes it."""
    key = native.get("partition_key")
    return Cookie(
        native["name"],
        native["value"],
        domain=native["domain"],
        path=native["path"],
        secure=native["secure"],
        http_only=native["http_only"],
        same_site=native["same_site"],
        priority=native["priority"],
        expires=_to_datetime(native.get("expires_us")),
        partition_key=None
        if key is None
        else CookiePartitionKey(key["top_level_site"], key["has_cross_site_ancestor"]),
        created=_to_datetime(native.get("created_us")),
        last_accessed=_to_datetime(native.get("last_accessed_us")),
        last_updated=_to_datetime(native.get("last_updated_us")),
        source_scheme=native["source_scheme"],
        source_port=native["source_port"],
        source_type=native["source_type"],
    )


def _replace_file(path: Path, contents: bytes) -> None:
    """Make `path` hold `contents`, with mode 0600, so that a crash leaves either the old file or the whole new one."""
    # mkstemp() gives mode 0600 whatever the umask, and the same directory keeps the replace on one file system.
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(descriptor, "wb") as file:
            file.write(contents)
            # Otherwise a power cut after the replace could leave an empty file there.
            file.flush()
            os.fsync(file.fileno())
        Path(temporary).replace(path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise
