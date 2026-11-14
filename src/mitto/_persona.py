"""Persona data: what real Chrome sends by default, for one Chrome Release on one operating system.

Each Persona is a file in the personas directory, written from the Baselines of its Chrome Release. See docs/persona.md
for the format, and for the parts of a request that come from //net rather than from this data. The values that follow the
machine are placeholders there, which a Session fills from its Machine.
"""

import dataclasses
import functools
import json
from collections.abc import Container, Iterable, Mapping
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any, NotRequired, Self, TypedDict

from mitto._body import EncodedBody, StreamBody
from mitto._machine import Machine, client_hints
from mitto._native import DeprecatedUnload, FetchParams, PageFeatures, Priority

__all__ = [
    "CHROME_155_WINDOWS",
    "CHROME_156_WINDOWS",
    "CHROME_157_WINDOWS",
    "ENGINE_RELEASE_PERSONA",
    "MACHINE_PLACEHOLDER",
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

MACHINE_PLACEHOLDER = "<machine>"
"""What a Persona's file holds for the value of a client hint that follows the machine."""

_PERSONAS_DIRECTORY = Path(__file__).with_name("personas")
_BOUNDARY_PLACEHOLDER = "<boundary>"
_SEC_FETCH_MODE = "Sec-Fetch-Mode"
_SEC_FETCH_DEST = "Sec-Fetch-Dest"
_SEC_FETCH_USER = "Sec-Fetch-User"


class RequestKind(StrEnum):
    """Which kind of Chrome request is being reproduced.

    Attributes:
        NAVIGATION: A navigation of the top-level page, such as a click on a link or a form's submission.
        FETCH: A page's fetch().
        SCRIPT: A classic script that an element asks for.
        IMAGE: An image that an element asks for.
        STYLE: A stylesheet that a link element or an @import asks for.
        FONT: A font that a stylesheet's @font-face asks for.
        MODULE: A module script, or what one imports.
        XHR: A page's XMLHttpRequest.
        WORKER: The script of a dedicated worker, which the browser asks for.
        BEACON: A page's navigator.sendBeacon().
        IFRAME: The navigation of a frame that an iframe element holds, whose parent is the Initiator.
        FAVICON: The icon of a page, which the browser asks for.
    """

    NAVIGATION = "navigation"
    FETCH = "fetch"
    SCRIPT = "script"
    IMAGE = "image"
    STYLE = "style"
    FONT = "font"
    MODULE = "module"
    XHR = "xhr"
    WORKER = "worker"
    BEACON = "beacon"
    IFRAME = "iframe"
    FAVICON = "favicon"


class RequestDefaults(TypedDict):
    """What Chrome sends by default for one request, as keyword arguments of the native Session's start() and request().

    Attributes:
        headers: The (name, value) pairs to send, in the order they reach the wire.
        priority: The priority of the request.
        priority_incremental: Whether the response can be used as it arrives.
        fetch: What the request says of itself, from which the native core sets some of those lines.
    """

    headers: tuple[tuple[str, str], ...]
    priority: Priority
    priority_incremental: bool
    fetch: FetchParams


class _BodyData(TypedDict):
    """What a body adds to one Request Kind in a Persona's file."""

    lines: list[str]
    content_types: dict[str, str]


class _KindData(TypedDict):
    """One Request Kind in a Persona's file."""

    credentials_mode: str
    priority: str
    priority_incremental: bool
    headers: list[str]
    body: NotRequired[_BodyData]


class _PageFeaturesData(TypedDict):
    """What a Page of the Persona turns on and off, in a Persona's file."""

    seed: str
    enable_blink_features: list[str]
    disable_blink_features: list[str]
    v8_flags: list[str]
    deprecated_unload: dict[str, Any]


class _PersonaData(TypedDict):
    """A Persona's file."""

    release: str
    operating_system: str
    accept_language: str
    user_agent: str
    kinds: dict[str, _KindData]
    page_features: _PageFeaturesData


@dataclass(frozen=True, slots=True)
class Persona:
    """The browser identity mitto presents to servers.

    Attributes:
        release: The Chrome Release, such as "157".
        operating_system: The operating system that the Persona claims, independent of the Host.
        accept_language: The Session's setting of the same name.
        user_agent: The native Session's setting of the same name, which is what its Chrome sends where no request's
            headers reach: in a CONNECT and in a fetch for a certificate.
        page_features: What a Page of this Persona turns on and off beside the Engine Release's runtime-enabled
            features and V8's defaults, so that it lacks what its Chrome Release did not ship and has what its
            variations seed switches.
    """

    release: str
    operating_system: str
    accept_language: str
    user_agent: str
    page_features: PageFeatures
    _kinds: dict[RequestKind, _KindData]

    @property
    def locale(self) -> str:
        """The BCP 47 tag of the locale that a page's Intl falls back to, such as "en-US".

        It is Chrome's language, which --lang sets for the Baselines, and which gives Accept-Language its first language.
        """
        return self.accept_language.split(",", 1)[0].split(";", 1)[0].strip()

    def request_defaults(self, kind: RequestKind, *, with_body: bool = False) -> RequestDefaults:
        """Return what this Persona's Chrome sends by default for a request of `kind`.

        The lines keep their places, and the native core gives some of them their values or removes them, from what
        `fetch` says and where the request goes.

        Args:
            kind: The Request Kind.
            with_body: Whether the request has a body, which adds lines. Whoever asks for this replaces the value of the
                Content-Type line, or removes the line, with content_type() or what a caller gives.
        """
        data = self._kinds[kind]
        only_with_a_body = () if with_body or "body" not in data else data["body"]["lines"]
        lines = _header_lines(data["headers"], only_with_a_body)
        values = dict(lines)
        return RequestDefaults(
            headers=lines,
            priority=Priority[data["priority"]],
            priority_incremental=data["priority_incremental"],
            fetch=FetchParams(
                mode=values[_SEC_FETCH_MODE],
                destination=values[_SEC_FETCH_DEST],
                credentials_mode=data["credentials_mode"],
                has_user_activation=_SEC_FETCH_USER in values,
            ),
        )

    def with_machine(self, machine: Machine) -> Self:
        """Return this Persona with the values of `machine` in place of the placeholder of each hint that follows it.

        Raises:
            ValueError: If the placeholder stands in a line that is no such hint.
        """
        values = client_hints(machine)
        kinds = {kind: _with_values(data, values) for kind, data in self._kinds.items()}
        return dataclasses.replace(self, _kinds=kinds)

    def takes_body(self, kind: RequestKind) -> bool:
        """Return whether Chrome sends a body with a request of `kind`, so that a body adds lines of its own."""
        return "body" in self._kinds[kind]

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
        user_agent=data["user_agent"],
        page_features=PageFeatures(
            enable_blink_features=tuple(data["page_features"]["enable_blink_features"]),
            disable_blink_features=tuple(data["page_features"]["disable_blink_features"]),
            v8_flags=tuple(data["page_features"]["v8_flags"]),
            deprecated_unload=DeprecatedUnload.from_option(data["page_features"]["deprecated_unload"]),
        ),
        _kinds={RequestKind(kind): kind_data for kind, kind_data in data["kinds"].items()},
    )


def _with_values(data: _KindData, values: Mapping[str, str]) -> _KindData:
    """Return the data of a Request Kind with `values`, by lower-case name, in place of each placeholder of the machine."""
    filled = data.copy()
    filled["headers"] = [_with_value(line, values) for line in data["headers"]]
    return filled


def _with_value(line: str, values: Mapping[str, str]) -> str:
    name, _, value = line.partition(": ")
    if value != MACHINE_PLACEHOLDER:
        return line
    if name.lower() not in values:
        raise ValueError(f"{name} holds {MACHINE_PLACEHOLDER}, and is no client hint that follows the machine")
    return f"{name}: {values[name.lower()]}"


def _header_lines(lines: Iterable[str], left_out: Container[str]) -> tuple[tuple[str, str], ...]:
    """Return the (name, value) pairs of a Persona's lines, but for those named in `left_out`."""
    pairs = (line.partition(": ") for line in lines)
    return tuple((name, value) for name, _, value in pairs if name not in left_out)
