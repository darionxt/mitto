"""Persona data: what real Chrome sends by default, for one Chrome Release on one operating system.

Each Persona is a file in the personas directory, written from the Baselines of its Chrome Release. See docs/persona.md
for the format, and for the parts of a request that come from //net rather than from this data.
"""

import functools
import json
from collections.abc import Container, Iterable
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import NotRequired, TypedDict
from urllib.parse import urlsplit

from mitto._body import EncodedBody, StreamBody
from mitto._native import Priority

__all__ = [
    "CHROME_155_WINDOWS",
    "CHROME_156_WINDOWS",
    "CHROME_157_WINDOWS",
    "ENGINE_RELEASE_PERSONA",
    "WINDOWS_PERSONAS",
    "Persona",
    "RequestDefaults",
    "RequestKind",
    "load_persona",
]

CHROME_155_WINDOWS = "chrome_155_windows"
CHROME_156_WINDOWS = "chrome_156_windows"
CHROME_157_WINDOWS = "chrome_157_windows"

WINDOWS_PERSONAS = {"155": CHROME_155_WINDOWS, "156": CHROME_156_WINDOWS, "157": CHROME_157_WINDOWS}
"""The file of the Persona of each Chrome Release in the Supported Window, on Windows."""

ENGINE_RELEASE_PERSONA = CHROME_157_WINDOWS
"""The Persona of the Engine Release, which a Session presents unless it is told otherwise."""

_PERSONAS_DIRECTORY = Path(__file__).with_name("personas")
_ORIGIN_PLACEHOLDER = "<origin>"
_BOUNDARY_PLACEHOLDER = "<boundary>"
_DEFAULT_PORTS = {"http": 80, "https": 443}


class RequestKind(StrEnum):
    """Which kind of Chrome request is being reproduced. Subresource kinds have a same-origin Initiator."""

    NAVIGATION = "navigation"
    FETCH = "fetch"
    SCRIPT = "script"
    IMAGE = "image"


class RequestDefaults(TypedDict):
    """What Chrome sends by default for one request, as keyword arguments of the native Session's start() and request().

    Attributes:
        headers: The (name, value) pairs to send, in the order they reach the wire.
        priority: The priority of the request.
        priority_incremental: Whether the response can be used as it arrives.
        referrer: The URL to send as the Referer header, or None to send none.
    """

    headers: tuple[tuple[str, str], ...]
    priority: Priority
    priority_incremental: bool
    referrer: str | None


class _BodyData(TypedDict):
    """What a body adds to one Request Kind in a Persona's file."""

    lines: list[str]
    content_types: dict[str, str]


class _KindData(TypedDict):
    """One Request Kind in a Persona's file."""

    priority: str
    priority_incremental: bool
    referrer: str | None
    headers: list[str]
    body: NotRequired[_BodyData]


class _PersonaData(TypedDict):
    """A Persona's file."""

    release: str
    operating_system: str
    accept_language: str
    kinds: dict[str, _KindData]


@dataclass(frozen=True, slots=True)
class Persona:
    """The browser identity mitto presents to servers.

    Attributes:
        release: The Chrome Release, such as "157".
        operating_system: The operating system that the Persona claims, independent of the Host.
        accept_language: The Session's setting of the same name.
    """

    release: str
    operating_system: str
    accept_language: str
    _kinds: dict[RequestKind, _KindData]

    def request_defaults(self, kind: RequestKind, url: str, *, with_body: bool = False) -> RequestDefaults:
        """Return what this Persona's Chrome sends by default for a request of `kind` to `url`.

        A subresource's Initiator is the page at the root of the origin of `url`, which is what its Referer names, and so
        is the page that sends a body, which is what its Origin names.

        Args:
            kind: The Request Kind.
            url: The URL to fetch.
            with_body: Whether the request has a body, which adds lines. Whoever asks for this replaces the value of the
                Content-Type line, or removes the line, with content_type() or what a caller gives.
        """
        data = self._kinds[kind]
        origin = _origin_of(url)
        referrer = data["referrer"]
        if referrer is not None:
            # A URL without an origin gets no referrer, which would be refused and hide what is wrong with the URL.
            referrer = None if origin is None else referrer.replace(_ORIGIN_PLACEHOLDER, origin)
        only_with_a_body = () if with_body or "body" not in data else data["body"]["lines"]
        return RequestDefaults(
            headers=_header_lines(data["headers"], only_with_a_body, origin),
            priority=Priority[data["priority"]],
            priority_incremental=data["priority_incremental"],
            referrer=referrer,
        )

    def content_type(self, kind: RequestKind, body: EncodedBody | StreamBody) -> str | None:
        """Return the Content-Type that this Persona's Chrome gives `body` on a request of `kind`, or None for none.

        Raises:
            ValueError: If Chrome sends no body with this Request Kind.
        """
        data = self._kinds[kind].get("body")
        if data is None:
            raise ValueError(f"Chrome sends no body with a request of the kind {kind.value!r}")
        if isinstance(body, StreamBody) or body.encoding is None:
            return None
        return data["content_types"][body.encoding].replace(_BOUNDARY_PLACEHOLDER, body.boundary)


@functools.cache
def load_persona(name: str) -> Persona:
    """Return the Persona in the file of this name, such as CHROME_157_WINDOWS.

    Raises:
        ValueError: If there is no Persona of this name.
    """
    if name not in WINDOWS_PERSONAS.values():
        raise ValueError(f"no Persona is named {name!r}, only {', '.join(WINDOWS_PERSONAS.values())}")
    data: _PersonaData = json.loads((_PERSONAS_DIRECTORY / f"{name}.json").read_text(encoding="utf-8"))
    return Persona(
        release=data["release"],
        operating_system=data["operating_system"],
        accept_language=data["accept_language"],
        _kinds={RequestKind(kind): kind_data for kind, kind_data in data["kinds"].items()},
    )


def _origin_of(url: str) -> str | None:
    """Return the origin of `url`, written as Chrome writes it, or None if it does not have one.

    So it holds no username or password, its host is in lower case and in ASCII, and it leaves out the scheme's default
    port. Unlike //net, this writes a name by IDNA 2003, which only differs for a few characters such as "ß".
    """
    try:
        parts = urlsplit(url)
        host = parts.hostname
        port = parts.port
        if not parts.scheme or not host:
            return None
        ascii_host = f"[{host}]" if ":" in host else host.encode("idna").decode()
    except ValueError:
        return None
    return f"{parts.scheme}://{ascii_host}" + ("" if port in (None, _DEFAULT_PORTS.get(parts.scheme)) else f":{port}")


def _header_lines(lines: Iterable[str], left_out: Container[str], origin: str | None) -> tuple[tuple[str, str], ...]:
    """Return the (name, value) pairs of a Persona's lines, but for those named in `left_out`.

    A line that names the origin needs one. Without it the line goes, as the referrer does.
    """
    pairs: list[tuple[str, str]] = []
    for line in lines:
        name, _, value = line.partition(": ")
        if name in left_out or (_ORIGIN_PLACEHOLDER in value and origin is None):
            continue
        pairs.append((name, value if origin is None else value.replace(_ORIGIN_PLACEHOLDER, origin)))
    return tuple(pairs)
