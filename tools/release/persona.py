"""Writing a Persona's file from the Baselines of its Chrome Release, the way docs/persona.md describes.

The lines of each Request Kind come from its Baselines over HTTP/1.1, which has the case Chrome writes, with what //net
adds taken away, and its priority from the priority header over HTTP/2. A hint that follows the machine gets a placeholder,
which a Session fills from its Machine. What no Baseline can hold comes from this module: the credentials mode of each
kind, and a Content-Type for a body that no Recording Case sends. Where a Baseline does hold a Content-Type, it has to
agree.
"""

import json
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path

from mitto._machine import MACHINE_HINTS
from mitto._native import Priority
from mitto._persona import MACHINE_PLACEHOLDER
from tools.capture import HTTP1, HTTP2
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE
from tools.capture.recording import Json
from tools.parity import BASELINES_DIRECTORY, Baseline, load_baselines

__all__ = [
    "PERSONAS_DIRECTORY",
    "PersonaDataError",
    "dumps_persona",
    "persona_data",
    "persona_path",
    "write_persona",
]

PERSONAS_DIRECTORY = Path(__file__).parents[2] / "src" / "mitto" / "personas"

OPERATING_SYSTEM = "Windows"

_BOUNDARY_PLACEHOLDER = "<boundary>"
_RECORDED_BOUNDARY = re.compile(r"boundary=\S+")

# What //net adds to a request, so a Persona has no line for it.
_ADDED_BY_NET = frozenset(
    {"host", "connection", "content-length", "referer", "accept-encoding", "accept-language", "cookie"}
)

# Lines whose place the Persona keeps, and whose value the native core sets from the request. A Persona's own case gives
# the value of Sec-Fetch-Site that goes without an Initiator.
_PLACEHOLDERS = {"content-type": "<content-type>", "origin": "<origin>"}
_CONTENT_TYPE = "content-type"
_SEC_FETCH_SITE = "sec-fetch-site"
_USER_AGENT = "user-agent"
_ACCEPT_LANGUAGE = "accept-language"
_PRIORITY = "priority"

# A request without a body can send Origin too, so it is not a line that only a body adds.
_NOT_ONLY_WITH_A_BODY = frozenset({"origin"})

# The urgency that //net leaves out of the priority header, and the highest it sends.
_DEFAULT_URGENCY = 3
_LAST_URGENCY = Priority.HIGHEST - Priority.THROTTLED
_INCREMENTAL = "i"
_URGENCY = re.compile(r"u=([0-9])")


@dataclass(frozen=True, slots=True)
class _BodySource:
    """What a body adds to one Request Kind of a Persona.

    Attributes:
        case: The Recording Case of a body. The lines that it has beyond the kind's own case are those only a body adds.
        content_types: The Content-Type of each kind of body.
    """

    case: str
    content_types: Mapping[str, str]


@dataclass(frozen=True, slots=True)
class _KindSource:
    """Which Recording Cases make one Request Kind of a Persona. The kind's own case has the kind's name.

    Attributes:
        credentials_mode: Whether the kind sends credentials, which no Baseline shows: as a page's fetch() does unless it
            asks otherwise, or as an element does.
        lines: The case whose lines, in their order, the kind's lines start from.
        hints: The case in which every hint goes, whose hints come after the lines of `lines`, or None if `lines` holds
            the hints in their places already.
        body: What a body adds, or None if the kind has no body.
    """

    credentials_mode: str
    lines: str
    hints: str | None = None
    body: _BodySource | None = None


# A navigation sends its hints in an order of its own. A subresource has its lines in the order they go with no hint, and
# then the hints, because the native core works out where each goes. See docs/persona.md.
_KINDS = {
    "navigation": _KindSource(
        credentials_mode="include",
        lines="client_hints_form",
        body=_BodySource(
            "navigation_form_urlencoded",
            {
                "text": "text/plain",
                "form": "application/x-www-form-urlencoded",
                "json": "application/json",
                "multipart": f"multipart/form-data; boundary={_BOUNDARY_PLACEHOLDER}",
            },
        ),
    ),
    "fetch": _KindSource(
        credentials_mode="same-origin",
        lines="fetch_post_text_same_origin",
        hints="client_hints_fetch_post",
        body=_BodySource(
            "fetch_post_text_same_origin",
            {
                "text": "text/plain;charset=UTF-8",
                "form": "application/x-www-form-urlencoded;charset=UTF-8",
                "json": "application/json",
                "multipart": f"multipart/form-data; boundary={_BOUNDARY_PLACEHOLDER}",
            },
        ),
    ),
    "script": _KindSource(credentials_mode="include", lines="script", hints="client_hints_script"),
    "image": _KindSource(credentials_mode="include", lines="image", hints="client_hints_image"),
}

# The Recording Cases that hold a Content-Type: of which kind, and for what kind of body.
_RECORDED_CONTENT_TYPES = {
    "navigation_form_urlencoded": ("navigation", "form"),
    "navigation_form_multipart": ("navigation", "multipart"),
    "fetch_post_text_same_origin": ("fetch", "text"),
    "fetch_post_text_cross_site": ("fetch", "text"),
    "fetch_post_json_same_origin": ("fetch", "json"),
}


class PersonaDataError(Exception):
    """The Baselines of a Chrome Release cannot make the file of its Persona."""


def persona_path(release: str, personas_directory: Path = PERSONAS_DIRECTORY) -> Path:
    """Return the file of the Persona of a Chrome Release, such as personas/chrome_157_windows.json."""
    return personas_directory / f"chrome_{release}_{OPERATING_SYSTEM.lower()}.json"


def persona_data(release: str, baselines: Iterable[Baseline]) -> dict[str, Json]:
    """Return the content of the Persona's file of a Chrome Release, from the Baselines of that Chrome Release.

    Args:
        release: The Chrome Release, such as "157".
        baselines: Baselines that include those of the Chrome Release. The others are left alone.

    Raises:
        PersonaDataError: A Recording Case that the file needs has no Baseline, a Baseline holds a Content-Type that is
            not this module's, a Machine has no value for a hint that follows the machine, or a priority is not one //net
            sends.
    """
    found = _Requests(release, baselines)
    _check_content_types(found)
    navigation = found.headers("navigation")
    return {
        "release": release,
        "operating_system": OPERATING_SYSTEM,
        "accept_language": _value(navigation, _ACCEPT_LANGUAGE),
        "user_agent": _value(navigation, _USER_AGENT),
        "kinds": {kind: _kind_data(found, kind, source) for kind, source in _KINDS.items()},
    }


def dumps_persona(data: Mapping[str, Json]) -> str:
    """Write the content of a Persona's file as text, the way the files in the personas directory have it."""
    return json.dumps(data, indent=2) + "\n"


def write_persona(
    release: str,
    baselines_directory: Path = BASELINES_DIRECTORY,
    personas_directory: Path = PERSONAS_DIRECTORY,
) -> Path:
    """Write the Persona's file of a Chrome Release from its Baselines, and return its path.

    Raises:
        PersonaDataError: See persona_data().
        BaselineError: A file under the Baselines directory is not a usable Baseline.
    """
    text = dumps_persona(persona_data(release, load_baselines(baselines_directory)))
    path = persona_path(release, personas_directory)
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


class _Requests:
    """The requests of the Baselines of one Chrome Release, by Recording Case and protocol."""

    def __init__(self, release: str, baselines: Iterable[Baseline]) -> None:
        self._release = release
        self._requests = {
            (baseline.case, baseline.protocol): baseline.fingerprint.get("request")
            for baseline in baselines
            if baseline.release == release
        }
        if not self._requests:
            raise PersonaDataError(f"Chrome {release} has no Baselines")

    def has(self, case: str) -> bool:
        return (case, HTTP1) in self._requests

    def headers(self, case: str, protocol: str = HTTP1) -> list[str]:
        """Return every header line of a case's request."""
        request = self._requests.get((case, protocol))
        headers = request.get("headers") if isinstance(request, dict) else None
        if not isinstance(headers, list):
            raise PersonaDataError(f"Chrome {self._release} has no Baseline of {case} over {protocol} with headers")
        return [line for line in headers if isinstance(line, str)]

    def lines(self, case: str) -> list[str]:
        """Return the header lines of a case's request over HTTP/1.1, but for those that //net adds."""
        return [line for line in self.headers(case) if _name(line) not in _ADDED_BY_NET]


def _kind_data(found: _Requests, kind: str, source: _KindSource) -> dict[str, Json]:
    lines = found.lines(source.lines)
    if source.hints is not None:
        lines = _with_hints(lines, found.lines(source.hints), source.hints)
    own = found.lines(kind)
    values = {**_PLACEHOLDERS, _SEC_FETCH_SITE: _value(own, _SEC_FETCH_SITE)}
    priority, incremental = _priority(found.headers(kind, HTTP2), kind)
    data: dict[str, Json] = {
        "credentials_mode": source.credentials_mode,
        "priority": priority.name,
        "priority_incremental": incremental,
        "headers": [_line(line, values) for line in lines],
    }
    if source.body is not None:
        body_names = {_name(line) for line in found.lines(source.body.case)}
        body_names -= {_name(line) for line in own} | _NOT_ONLY_WITH_A_BODY
        data["body"] = {
            "lines": [line.partition(":")[0] for line in lines if _name(line) in body_names],
            "content_types": dict(source.body.content_types),
        }
    return data


def _with_hints(lines: list[str], hinted: list[str], case: str) -> list[str]:
    """Return the lines with the hints from the case where every hint goes, ahead of what Chrome sends after them."""
    names = {_name(line) for line in lines}
    places = [number for number, line in enumerate(hinted) if _name(line) not in names]
    if not places:
        raise PersonaDataError(f"{case} has no hint that the kind's own Baseline does not have")
    after = {_name(line) for line in hinted[places[-1] + 1 :]}
    at = next((number for number, line in enumerate(lines) if _name(line) in after), len(lines))
    return [*lines[:at], *(hinted[number] for number in places), *lines[at:]]


def _line(line: str, values: Mapping[str, str]) -> str:
    name, _, value = line.partition(": ")
    lower_name = name.lower()
    if lower_name in MACHINE_HINTS:
        return f"{name}: {MACHINE_PLACEHOLDER}"
    if lower_name in HINTS_FROM_THE_MACHINE:
        raise PersonaDataError(f"{name} follows the machine, and a Machine has no value for it")
    return f"{name}: {values.get(lower_name, value)}"


def _priority(lines: list[str], kind: str) -> tuple[Priority, bool]:
    """Return the priority and whether it is incremental, from the priority header that //net sent over HTTP/2."""
    field = next((line.partition(":")[2].strip() for line in lines if _name(line) == _PRIORITY), "")
    parts = [part.strip() for part in field.split(",") if part.strip()]
    urgency = _DEFAULT_URGENCY
    for part in parts:
        if match := _URGENCY.fullmatch(part):
            urgency = int(match[1])
        elif part != _INCREMENTAL:
            raise PersonaDataError(f"the priority of {kind} over HTTP/2 is not one //net sends: {field!r}")
    if urgency > _LAST_URGENCY:
        raise PersonaDataError(f"the priority of {kind} over HTTP/2 has an urgency //net does not send: {field!r}")
    return Priority(Priority.HIGHEST - urgency), _INCREMENTAL in parts


def _check_content_types(found: _Requests) -> None:
    for case, (kind, body) in _RECORDED_CONTENT_TYPES.items():
        source = _KINDS[kind].body
        if source is None or not found.has(case):
            continue
        recorded = _value(found.lines(case), _CONTENT_TYPE)
        recorded = _RECORDED_BOUNDARY.sub(f"boundary={_BOUNDARY_PLACEHOLDER}", recorded)
        if recorded != source.content_types[body]:
            raise PersonaDataError(
                f"{case} sends Content-Type: {recorded}, where a Persona's {kind} says {source.content_types[body]}"
            )


def _value(lines: Iterable[str], name: str) -> str:
    for line in lines:
        if _name(line) == name:
            return line.partition(":")[2].strip()
    raise PersonaDataError(f"a Baseline has no {name} line where a Persona needs one")


def _name(line: str) -> str:
    return line.partition(":")[0].lower()
