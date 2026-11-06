"""Persona data: what real Chrome sends by default, for one Chrome Release on one operating system.

Each Persona is a file in the personas directory, written from the Baselines of its Chrome Release. See docs/persona.md
for the format, and for the parts of a request that come from //net rather than from this data.
"""

import functools
import json
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import TypedDict
from urllib.parse import urlsplit

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


class _KindData(TypedDict):
    """One Request Kind in a Persona's file."""

    priority: str
    priority_incremental: bool
    referrer: str | None
    headers: list[str]


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

    def request_defaults(self, kind: RequestKind, url: str) -> RequestDefaults:
        """Return what this Persona's Chrome sends by default for a request of `kind` to `url`.

        A subresource's Initiator is the page at the root of the origin of `url`, which is what its Referer names.
        """
        data = self._kinds[kind]
        referrer = data["referrer"]
        if referrer is not None:
            origin = _origin_of(url)
            # A URL without an origin gets no referrer, which would be refused and hide what is wrong with the URL.
            referrer = None if origin is None else referrer.replace(_ORIGIN_PLACEHOLDER, origin)
        return RequestDefaults(
            headers=tuple(_parse_header(line) for line in data["headers"]),
            priority=Priority[data["priority"]],
            priority_incremental=data["priority_incremental"],
            referrer=referrer,
        )


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
    """Return the scheme and authority of `url`, or None if it does not have both."""
    try:
        parts = urlsplit(url)
    except ValueError:
        return None
    return f"{parts.scheme}://{parts.netloc}" if parts.scheme and parts.netloc else None


def _parse_header(line: str) -> tuple[str, str]:
    name, _, value = line.partition(": ")
    return name, value
