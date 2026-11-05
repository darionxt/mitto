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

__all__ = ["CHROME_157_WINDOWS", "Persona", "RequestDefaults", "RequestKind", "load_persona"]

CHROME_157_WINDOWS = "chrome_157_windows"

_PERSONAS_DIRECTORY = Path(__file__).with_name("personas")
_ORIGIN_PLACEHOLDER = "<origin>"


class RequestKind(StrEnum):
    """Which kind of Chrome request is being reproduced. Subresource kinds have a same-origin Initiator."""

    NAVIGATION = "navigation"
    FETCH = "fetch"
    SCRIPT = "script"
    IMAGE = "image"


class RequestDefaults(TypedDict):
    """What Chrome sends by default for one request, as the keyword arguments of Session.request().

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
            parts = urlsplit(url)
            referrer = referrer.replace(_ORIGIN_PLACEHOLDER, f"{parts.scheme}://{parts.netloc}")
        return RequestDefaults(
            headers=tuple(_parse_header(line) for line in data["headers"]),
            priority=Priority[data["priority"]],
            priority_incremental=data["priority_incremental"],
            referrer=referrer,
        )


@functools.cache
def load_persona(name: str) -> Persona:
    """Return the Persona in the file of this name, such as CHROME_157_WINDOWS."""
    data: _PersonaData = json.loads((_PERSONAS_DIRECTORY / f"{name}.json").read_text(encoding="utf-8"))
    return Persona(
        release=data["release"],
        operating_system=data["operating_system"],
        accept_language=data["accept_language"],
        _kinds={RequestKind(kind): kind_data for kind, kind_data in data["kinds"].items()},
    )


def _parse_header(line: str) -> tuple[str, str]:
    name, _, value = line.partition(": ")
    return name, value
