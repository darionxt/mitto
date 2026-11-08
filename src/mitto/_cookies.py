"""A Session's cookies: what the cookie store in //net holds, and a file to keep them in with the Session's Machine.

//net decides which cookies a response sets and which a request sends. This module only reads and changes that store,
through the native core, so that nothing here can disagree with it. See docs/parity.md for how a Session uses them.
"""

import asyncio
import json
import os
from collections.abc import Iterator
from dataclasses import KW_ONLY, dataclass, field
from datetime import UTC, datetime, timedelta
from types import UnionType
from typing import Any, Literal

from mitto import _native
from mitto._arguments import require_path, require_type
from mitto._cookie_file import CookieFile, read_cookie_file, write_cookie_file
from mitto._machine import Machine

__all__ = ["AsyncCookies", "Cookie", "CookiePartitionKey", "Cookies"]

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

    Raises:
        TypeError: If `top_level_site` is not text, or `has_cross_site_ancestor` is neither True nor False.
    """

    top_level_site: str
    has_cross_site_ancestor: bool

    def __post_init__(self) -> None:
        require_type("top_level_site", self.top_level_site, str, "text")
        require_type("has_cross_site_ancestor", self.has_cross_site_ancestor, bool, "True or False")


_TIME = "a datetime with a timezone or None"
_TIMES = ("expires", "created", "last_accessed", "last_updated")

# What each attribute of a Cookie takes. Text outside the values of its Literal type is the native core's to refuse.
_COOKIE_ATTRIBUTES: tuple[tuple[str, type | UnionType, str], ...] = (
    ("name", str, "text"),
    ("value", str, "text"),
    ("domain", str, "text"),
    ("path", str, "text"),
    ("secure", bool, "True or False"),
    ("http_only", bool, "True or False"),
    ("same_site", str, "'unspecified', 'none', 'lax' or 'strict'"),
    ("priority", str, "'low', 'medium' or 'high'"),
    ("expires", datetime | None, _TIME),
    ("partition_key", CookiePartitionKey | None, "a CookiePartitionKey or None"),
    ("created", datetime | None, _TIME),
    ("last_accessed", datetime | None, _TIME),
    ("last_updated", datetime | None, _TIME),
    ("source_scheme", str | None, "'unset', 'non_secure', 'secure' or None"),
    ("source_port", int | None, "a whole number or None"),
    ("source_type", str | None, "'http', 'script', 'other' or None"),
)


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

    Raises:
        TypeError: If an attribute is of a type it does not take, or a time has no timezone, which the message names.
            True and False are no port.
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

    def __post_init__(self) -> None:
        for argument, expected, what in _COOKIE_ATTRIBUTES:
            require_type(argument, getattr(self, argument), expected, what)
        for argument in _TIMES:
            time: datetime | None = getattr(self, argument)
            # A time without one could be in any timezone, and the store keeps each in UTC.
            if time is not None and time.utcoffset() is None:
                raise TypeError(f"{argument} has to be {_TIME}, not a datetime without one")
        # True and False are ints to Python, but no port.
        if isinstance(self.source_port, bool):
            raise TypeError("source_port has to be a whole number or None, not bool")


class _CookieStore:
    """What the cookie store in //net holds, for each API's kind of cookies to read and change.

    Each call asks the network thread, and blocks only the calling thread for as long as that takes, with the GIL
    released. The calls may come from any thread, but not after the Session has closed, when they raise a ValueError.
    """

    def __init__(self, native: _native.Session, machine: Machine) -> None:
        self._native = native
        self._machine = machine

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

        Raises:
            TypeError: If `name` is not text, or `domain` or `path` is neither text nor None.
        """
        _require_keys(name, domain, path)
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
            TypeError: If `cookie` is not a Cookie.
        """
        require_type("cookie", cookie, Cookie, "a Cookie")
        self._native.set_cookies(
            json.dumps({"version": _native.COOKIES_VERSION, "cookies": [_to_native(cookie)]}).encode()
        )

    def delete(self, name: str, *, domain: str | None = None, path: str | None = None) -> int:
        """Delete the cookies of this name, and return how many went.

        Args:
            name: The name.
            domain: The domain, written as Cookie.domain is, or None for any.
            path: The path, or None for any.

        Raises:
            TypeError: If `name` is not text, or `domain` or `path` is neither text nor None.
        """
        _require_keys(name, domain, path)
        keys = {"name": name, "domain": domain, "path": path}
        return self._delete({key: value for key, value in keys.items() if value is not None})

    def clear(self) -> int:
        """Delete every cookie, and return how many went."""
        return self._delete({})

    def _add_from(self, file: CookieFile) -> None:
        """Add the cookies of a file, which has to hold this Session's Machine.

        Raises:
            ValueError: If the file holds another Machine, or the native core refuses its cookies. Either way, no cookie
                is added.
        """
        if file.machine != self._machine:
            raise ValueError(
                f"{file.path} holds the cookies of another Machine than this Session claims: open a Session with"
                " cookie_file= to claim the file's"
            )
        file.add_to(self._native)

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
        """Write all the cookies, with the Session's Machine, to a JSON file that only its owner can read or write.

        The values are not encrypted. Session cookies are written too, as a browser does that restores its last session.
        A crash during this call leaves the file as it was.

        Raises:
            TypeError: If `path` is neither text nor a path.
            OSError: If the file cannot be written.
        """
        require_path("path", path)
        write_cookie_file(path, self._machine, self._native.list_cookies())

    def load(self, path: str | os.PathLike[str]) -> None:
        """Add the cookies of a file from save(), each with the attributes it had there, replacing any of the same name.

        The file has to hold the Machine that this Session claims. A file from mitto 0.1 holds Machine.named("mitto-0.1").

        Raises:
            TypeError: If `path` is neither text nor a path.
            OSError: If the file cannot be read, such as a FileNotFoundError.
            ValueError: If the file holds no cookies that this Session can use, or holds another Machine, in which case
                no cookie is added.
        """
        require_path("path", path)
        self._add_from(read_cookie_file(path))


class AsyncCookies(_CookieStore):
    """The cookies of one AsyncSession, which are those of Cookies with the file left to a thread of the event loop's.

    What the network thread answers is one short task on it, which never waits for Python, so those calls stay plain ones,
    on the event loop's thread. They raise a ValueError once the AsyncSession has begun to close.
    """

    async def save(self, path: str | os.PathLike[str]) -> None:
        """Write all the cookies to a file off the event loop, as Cookies.save() does.

        Raises:
            TypeError: As Cookies.save() does.
            OSError: If the file cannot be written.
        """
        require_path("path", path)
        # This asks the network thread here, so that nothing off the loop can use a native Session that is closing.
        await asyncio.to_thread(write_cookie_file, path, self._machine, self._native.list_cookies())

    async def load(self, path: str | os.PathLike[str]) -> None:
        """Read a file from save() off the event loop and add its cookies, as Cookies.load() does.

        Raises:
            TypeError: As Cookies.load() does.
            OSError: If the file cannot be read, such as a FileNotFoundError.
            ValueError: As Cookies.load() does, in which case no cookie is added.
        """
        require_path("path", path)
        self._add_from(await asyncio.to_thread(read_cookie_file, path))


def _require_keys(name: object, domain: object, path: object) -> None:
    """Refuse what picks out cookies, unless `name` is text, and `domain` and `path` are text or None.

    Raises:
        TypeError: If one of them is of another type.
    """
    require_type("name", name, str, "text")
    require_type("domain", domain, str | None, "text or None")
    require_type("path", path, str | None, "text or None")


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
