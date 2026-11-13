"""The Normal Form of a Script Fingerprint: the probe's record with what the machine decides hidden as <machine>, what
changes from run to run as <run>, and the page's origin behind placeholders.

Two recordings of one Chrome Release then compare equal, and no value of the recording machine reaches the repository.
What the Persona decides stays, and so do the interfaces, their members and order, and what each check gives.
docs/capture.md, "The Normal Form of a Script Fingerprint", says what it hides, keeps and drops, and why.
"""

import json
import re
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from typing import cast
from urllib.parse import urlsplit

from tools.capture.cases import CROSS_SITE_HOST, SAME_SITE_HOST
from tools.capture.normal_form import (
    AUTHORITY_PLACEHOLDER,
    CROSS_SITE_AUTHORITY_PLACEHOLDER,
    CROSS_SITE_ORIGIN_PLACEHOLDER,
    HOST_PLACEHOLDER,
    MACHINE_PLACEHOLDER,
    ORIGIN_PLACEHOLDER,
    SAME_SITE_AUTHORITY_PLACEHOLDER,
    SAME_SITE_ORIGIN_PLACEHOLDER,
)
from tools.capture.probe import RecordValue, dumps_record, read_record

RUN_PLACEHOLDER = "<run>"
"""What a Normal Form holds for a value that changes between two runs of one Chrome on one machine."""

PORT_PLACEHOLDER = "<port>"
"""What a Normal Form holds for the page's port, where a value is the port alone, as location.port is."""

SAME_SITE_HOST_PLACEHOLDER = "<same-site-host>"
"""What a Normal Form holds for the name of another origin of the page's site, where a value is the name alone, as
location.hostname is."""

CROSS_SITE_HOST_PLACEHOLDER = "<cross-site-host>"
"""What a Normal Form holds for the name of another site, where a value is the name alone, as location.hostname is in a
frame of that site."""


@dataclass(frozen=True, slots=True)
class Getter:
    """What a getter gave on an object that the walk reached: in the record, the item of the object's "get" named name.

    Attributes:
        path: The object's path, such as "navigator.connection".
        name: The getter's key, or None for every getter of the object.
    """

    path: str
    name: str | None

    def __str__(self) -> str:
        return self.path if self.name is None else f"{self.path}.{self.name}"


@dataclass(frozen=True, slots=True)
class Computed:
    """One of the values that the probe works out in its third step, or a part of it: in the record, the "value" of
    values[key], and in that the item of each of keys in turn.

    Attributes:
        key: The value's key in the record's "values", such as "new Intl.DateTimeFormat().resolvedOptions()".
        keys: The keys of the part within that value, such as ("timeZone",), or nothing for all of it.
    """

    key: str
    keys: tuple[str, ...] = ()

    def __str__(self) -> str:
        return "".join((self.key, *(f"[{json.dumps(key)}]" for key in self.keys)))


type Place = Getter | Computed
"""A place in a record, which a Normal Form hides."""


def _getters(path: str, *names: str) -> tuple[Getter, ...]:
    return tuple(Getter(path, name) for name in names)


def _parts(key: str, *names: str) -> tuple[Computed, ...]:
    return tuple(Computed(key, (name,)) for name in names)


# The keys of the values of the probe's third step that hold some of the machine's.
HIGH_ENTROPY_VALUES = "navigator.userAgentData.getHighEntropyValues()"
MEDIA_KEYWORDS = "matchMedia(): the keywords that match"
MEDIA_INTEGERS = "matchMedia(): the integers that match"
MEDIA_AGREEMENT = "matchMedia(): what agrees with the window and the screen"
DATE_TIME_FORMAT = "new Intl.DateTimeFormat().resolvedOptions()"
DATE_TIME_FORMAT_HOURS = 'new Intl.DateTimeFormat(undefined, {hour: "numeric"}).resolvedOptions()'
TIME_ZONE_OFFSETS = "Date: getTimezoneOffset() at noon UTC on the first of each month"
DATE_STRINGS = "Date: the strings of a winter day and a summer day"
DATE_STRING_NAMES = (
    "toString",
    "toTimeString",
    "toLocaleString",
    "toLocaleDateString",
    "toLocaleTimeString",
    "timeZoneName long",
)

MACHINE_PLACES: tuple[Place, ...] = (
    # The viewport, the window and the display, its scale, depth, gamut and range, and whether there are more of them.
    *_getters(
        "globalThis",
        "innerWidth",
        "innerHeight",
        "outerWidth",
        "outerHeight",
        "screenX",
        "screenY",
        "screenLeft",
        "screenTop",
        "devicePixelRatio",
    ),
    *_getters(
        "screen",
        "availWidth",
        "availHeight",
        "width",
        "height",
        "colorDepth",
        "pixelDepth",
        "availLeft",
        "availTop",
        "isExtended",
    ),
    *_getters("screen.orientation", "type", "angle"),
    *_getters("visualViewport", "width", "height"),
    *_getters('viewport.segments["0"]', "width", "height", "right", "bottom"),
    # The processors, the memory, the touch screen and the network.
    *_getters("navigator", "hardwareConcurrency", "deviceMemory", "maxTouchPoints", "cpuPerformance"),
    *_getters("navigator.connection", "effectiveType", "rtt", "downlink"),
    Getter("console.memory", "jsHeapSizeLimit"),
    Getter("performance.memory", "jsHeapSizeLimit"),
    Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)),
    # What the media queries read of the machine and of its user's settings, also for a feature that a Chrome Release
    # does not parse, which then matches nothing.
    *_parts(
        MEDIA_KEYWORDS,
        "display-state",
        "orientation",
        "pointer",
        "any-pointer",
        "hover",
        "any-hover",
        "color-gamut",
        "inverted-colors",
        "prefers-color-scheme",
        "prefers-contrast",
        "prefers-reduced-motion",
        "dynamic-range",
        "video-dynamic-range",
        "prefers-reduced-transparency",
        "forced-colors",
    ),
    Computed(MEDIA_INTEGERS, ("color",)),
    # Whether the media queries' sizes agree with the window's and the screen's, which the window and the screen give in
    # whole device-independent pixels: they do not where those are not whole, as with some scales.
    *_parts(
        MEDIA_AGREEMENT,
        "width = innerWidth",
        "height = innerHeight",
        "aspect-ratio = innerWidth / innerHeight",
        "device-width = screen.width",
        "device-height = screen.height",
        "device-aspect-ratio = screen.width / screen.height",
    ),
    # The time zone.
    Computed(DATE_TIME_FORMAT, ("timeZone",)),
    Computed(DATE_TIME_FORMAT_HOURS, ("timeZone",)),
    Computed(TIME_ZONE_OFFSETS),
    *(Computed(DATE_STRINGS, (day, name)) for day in ("winter", "summer") for name in DATE_STRING_NAMES),
)
"""Each place whose value follows the machine, which a Normal Form hides as <machine>. machine_forms.SCRIPT_FORMS holds
the form of each.

These are a top-level page's. A frame has those of FRAME_MACHINE_PLACES, and a worker those of WORKER_MACHINE_PLACES, at
the same places.
"""

FRAME_VIEWPORT: Mapping[Place, RecordValue] = {
    Getter("globalThis", "innerWidth"): 300,
    Getter("globalThis", "innerHeight"): 150,
    Getter("visualViewport", "width"): 300,
    Getter("visualViewport", "height"): 150,
    Computed(MEDIA_KEYWORDS, ("orientation",)): ["landscape"],
    Computed(MEDIA_AGREEMENT, ("width = innerWidth",)): True,
    Computed(MEDIA_AGREEMENT, ("height = innerHeight",)): True,
    Computed(MEDIA_AGREEMENT, ("aspect-ratio = innerWidth / innerHeight",)): True,
}
"""What a frame's viewport gives at the places of MACHINE_PLACES that are a frame's own: an iframe's size by default,
300 by 150 pixels, whatever the machine, which the case decides. A frame has no viewport segments."""

_A_FRAMES_OWN: frozenset[Place] = frozenset(
    {*FRAME_VIEWPORT, *_getters('viewport.segments["0"]', "width", "height", "right", "bottom")}
)

FRAME_MACHINE_PLACES: tuple[Place, ...] = tuple(place for place in MACHINE_PLACES if place not in _A_FRAMES_OWN)
"""Each place whose value follows the machine in a frame: those of MACHINE_PLACES but the frame's viewport, its size
and shape, which its element in the parent decides and which the Normal Form keeps, such as innerWidth."""

_IN_A_WORKER = frozenset(
    {
        *_getters("navigator", "hardwareConcurrency", "deviceMemory"),
        *_getters("navigator.connection", "effectiveType", "rtt", "downlink"),
    }
)
_COMPUTED_IN_A_WORKER = frozenset(
    {HIGH_ENTROPY_VALUES, DATE_TIME_FORMAT, DATE_TIME_FORMAT_HOURS, TIME_ZONE_OFFSETS, DATE_STRINGS}
)

WORKER_MACHINE_PLACES: tuple[Place, ...] = tuple(
    place
    for place in MACHINE_PLACES
    if place in _IN_A_WORKER or (isinstance(place, Computed) and place.key in _COMPUTED_IN_A_WORKER)
)
"""Each place whose value follows the machine in a dedicated worker: the processors, the memory, the network, Windows'
version and the time zone. A worker has no window, no screen, no matchMedia() and no memory figures."""

RUN_PLACES: tuple[Place, ...] = (
    Getter("performance", "timeOrigin"),
    Getter("performance.timing", None),
    *_getters("performance.memory", "usedJSHeapSize", "totalJSHeapSize"),
    Getter("document", "lastModified"),
    Getter("document.timeline", "currentTime"),
    *_getters("navigation.currentEntry", "key", "id"),
)
"""Each place whose value changes from one run of a Chrome to the next on one machine, which a Normal Form hides as
<run>.

#207 found them over 11 runs of Chrome 157: the times, document.lastModified, which is the time the page loaded when
its response has no Last-Modified, and the navigation entry's key and id, which are random. performance.memory gives
precise figures in a renderer that is locked to a site, as each is on the desktop, at most every 50 ms, from a cache
that console.memory's rounded figures share (WindowPerformance::memory() and HeapSizeCache in Blink's core/timing), so
what it gives depends on how long the walk took after it read console.memory (#210)."""


def script_normal_form(
    record: dict[str, RecordValue], page_url: str | None, machine_places: Iterable[Place] = MACHINE_PLACES
) -> dict[str, RecordValue]:
    """Return the Normal Form of a probe's record, as a new record.

    A getter that threw at one of those places stays as it is, since how it failed is the Runtime's and not the
    machine's.

    Args:
        record: What the probe gave, as read_record() reads it.
        page_url: The address of the top-level page, whose origin, name and port go behind placeholders, with those of
            the other origin of its site and of the other site that a frame of the page can be at, or None for a page
            with no origin of its own, such as about:blank.
        machine_places: Each place whose value follows the machine where the probe ran: MACHINE_PLACES in a top-level
            page, FRAME_MACHINE_PLACES in a frame and WORKER_MACHINE_PLACES in a worker.
    """
    form = cast(dict[str, RecordValue], json.loads(json.dumps(record)))
    _hide(form, RUN_PLACES, RUN_PLACEHOLDER)
    _hide(form, machine_places, MACHINE_PLACEHOLDER)
    if page_url is None:
        return form
    return cast(dict[str, RecordValue], _behind_placeholders(form, _Addresses.of(page_url)))


def machine_values(
    record: dict[str, RecordValue], places: Iterable[Place] = MACHINE_PLACES
) -> Iterator[tuple[Place, RecordValue]]:
    """Yield each place of the machine that a record holds, with its value there, in the order of the places."""
    for place in places:
        for slot in _found(record, place):
            yield place, slot.get()


def machine_values_shown(
    raw_record: dict[str, RecordValue],
    form: dict[str, RecordValue],
    places: Iterable[Place] = MACHINE_PLACES,
    own: Iterable[Place] = (),
) -> tuple[int, list[str]]:
    """Look for each value of the machine that a raw record holds anywhere in a Normal Form, such as its own.

    A Normal Form hides each value of MACHINE_PLACES, but a value that a place left out holds, or that another value is
    made of, would show elsewhere. So this looks for each value that only a machine would give: text of four characters
    or more that is no keyword of Chrome's, such as a time zone's name or a date with its zone, a number of 100 or more,
    such as a size or a heap limit, and a list of three numbers or more, such as the time zone's offsets. A string is
    looked for in each string, and a number among what the getters and the third step gave, where it cannot be one of
    the constants that interfaces hold.

    Args:
        raw_record: What the probe gave.
        form: Its Normal Form, or what of it a Baseline keeps.
        places: Each place whose value follows the machine where the probe ran.
        own: Each place whose value the case decides and not the machine, such as a frame's size, which is not looked
            in, since a value there, such as a height of 150, can be the same number as one of the machine's by chance.

    Returns:
        How many values it looked for, and where in the Normal Form one showed, such as "screen get width". It never
        says which value showed.
    """
    sought: list[RecordValue] = []
    for _, value in machine_values(raw_record, places):
        for candidate in _distinctive(value):
            if candidate not in sought:
                sought.append(candidate)
    skipped = {_where(place) for place in own}
    shown = [
        where
        for where, value, numbers_count in _leaves(form)
        if where not in skipped and any(_shows(candidate, value, numbers_count=numbers_count) for candidate in sought)
    ]
    return len(sought), list(dict.fromkeys(shown))


def _where(place: Place) -> str:
    """Return where _leaves() says a value at a place is."""
    if isinstance(place, Getter):
        return f"{place.path} get {place.name}"
    return "".join((f'values[{json.dumps(place.key)}]["value"]', *(f"[{json.dumps(key)}]" for key in place.keys)))


_KEYWORD_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789-")
_SHORTEST_DISTINCTIVE_TEXT = 4
_SHORTEST_TEXT_TO_FIND_INSIDE = 6
_LEAST_DISTINCTIVE_NUMBER = 100
_SHORTEST_DISTINCTIVE_LIST = 3


def _distinctive(value: RecordValue) -> list[RecordValue]:
    """Return what of a value of the machine only a machine would give, which a Normal Form must not show."""
    if isinstance(value, str):
        is_keyword = set(value) <= _KEYWORD_CHARACTERS
        return [value] if len(value) >= _SHORTEST_DISTINCTIVE_TEXT and not is_keyword else []
    if _is_number(value):
        return [value] if _is_large(value) else []
    if isinstance(value, list) and len(value) >= _SHORTEST_DISTINCTIVE_LIST and all(_is_number(item) for item in value):
        return [value, *(item for item in value if _is_large(item))]
    return []


def _is_number(value: RecordValue) -> bool:
    return isinstance(value, int | float) and not isinstance(value, bool)


def _is_large(value: RecordValue) -> bool:
    return isinstance(value, int | float) and not isinstance(value, bool) and abs(value) >= _LEAST_DISTINCTIVE_NUMBER


def _shows(candidate: RecordValue, value: RecordValue, *, numbers_count: bool) -> bool:
    if isinstance(candidate, str):
        if not isinstance(value, str):
            return False
        return value == candidate or (len(candidate) >= _SHORTEST_TEXT_TO_FIND_INSIDE and candidate in value)
    if isinstance(candidate, list):
        return isinstance(value, list) and value == candidate
    return numbers_count and _is_number(value) and value == candidate


def _leaves(form: dict[str, RecordValue]) -> Iterator[tuple[str, RecordValue, bool]]:
    """Yield each value in a record and each list in it, with where it is and whether a number there may be the
    machine's."""
    for entry in _objects(form):
        path = entry.get("path")
        for key, value in entry.items():
            if key in ("own", "get", "calls") and isinstance(value, list):
                for item in value:
                    if not isinstance(item, list) or not item:
                        continue
                    for part in item[1:]:
                        yield from _inside(f"{path} {key} {item[0]}", part, numbers_count=key == "get")
            elif key != "path":
                yield from _inside(f"{path} {key}", value, numbers_count=False)
    values = form.get("values")
    if isinstance(values, dict):
        for key, value in values.items():
            yield from _inside(f"values[{json.dumps(key)}]", value, numbers_count=True)
    yield from _inside("reports", form.get("reports"), numbers_count=False)


def _inside(where: str, value: RecordValue, *, numbers_count: bool) -> Iterator[tuple[str, RecordValue, bool]]:
    if isinstance(value, dict):
        for key, item in value.items():
            yield from _inside(f"{where}[{json.dumps(key)}]", item, numbers_count=numbers_count)
        return
    yield where, value, numbers_count
    if isinstance(value, list):
        for item in value:
            yield from _inside(where, item, numbers_count=numbers_count)


@dataclass(frozen=True, slots=True)
class ScriptRawRecord:
    """What a recording session keeps of a run of a Script Fingerprint, on the machine that recorded it.

    Attributes:
        url: The address of the page that the probe ran in.
        peers: The address of each client that reached the page's route, which is loopback when nothing went elsewhere.
        record: What the probe gave.
    """

    url: str
    peers: tuple[str, ...]
    record: dict[str, RecordValue]


def dumps_script_raw_record(url: str, peers: Iterable[str], record: dict[str, RecordValue]) -> str:
    """Write what a recording session keeps of a run of a Script Fingerprint, with the record as dumps_record() writes
    it."""
    head = json.dumps({"url": url, "peers": list(peers)}, ensure_ascii=False)[:-1]
    return f'{head},\n"record": {dumps_record(record).rstrip()}\n}}\n'


def read_script_raw_record(text: str) -> ScriptRawRecord:
    """Read what dumps_script_raw_record() wrote.

    Raises:
        ValueError: If the text is not what it writes.
    """
    value: object = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("this is not a raw record of a Script Fingerprint")
    raw = cast(dict[str, object], value)
    url, peers, record = raw.get("url"), raw.get("peers"), raw.get("record")
    if not isinstance(url, str) or not isinstance(peers, list) or not isinstance(record, dict):
        raise ValueError("this is not a raw record of a Script Fingerprint")
    return ScriptRawRecord(url, tuple(str(peer) for peer in cast(list[object], peers)), read_record(json.dumps(record)))


def _hide(record: dict[str, RecordValue], places: Iterable[Place], placeholder: str) -> None:
    for place in places:
        for slot in _found(record, place):
            # A getter that threw is no value of the machine or of a run, and how it failed is for a Parity Check to see.
            if not _is_failure(slot.get()):
                slot.put(placeholder)


def _is_failure(value: RecordValue) -> bool:
    """Whether a value is the probe's ["throws", message]."""
    return isinstance(value, list) and len(value) == 2 and value[0] == "throws"


@dataclass(frozen=True, slots=True)
class _GetterSlot:
    """Where a getter's value is in a record: the second item of its [key, value]."""

    item: list[RecordValue]

    def get(self) -> RecordValue:
        return self.item[1]

    def put(self, value: RecordValue) -> None:
        self.item[1] = value


@dataclass(frozen=True, slots=True)
class _ValueSlot:
    """Where a value of the third step, or a part of one, is in a record."""

    container: dict[str, RecordValue]
    key: str

    def get(self) -> RecordValue:
        return self.container[self.key]

    def put(self, value: RecordValue) -> None:
        self.container[self.key] = value


def _found(record: dict[str, RecordValue], place: Place) -> Iterator[_GetterSlot | _ValueSlot]:
    """Yield where each value at a place is in a record. A place that the record lacks yields nothing."""
    if isinstance(place, Getter):
        for entry in _objects(record):
            getters = entry.get("get")
            if entry.get("path") != place.path or not isinstance(getters, list):
                continue
            for item in getters:
                if isinstance(item, list) and len(item) == 2 and (place.name is None or item[0] == place.name):
                    yield _GetterSlot(item)
        return
    values = record.get("values")
    entry = values.get(place.key) if isinstance(values, dict) else None
    if not isinstance(entry, dict) or "value" not in entry:
        return
    slot = _ValueSlot(entry, "value")
    for part in place.keys:
        inner = slot.get()
        if not isinstance(inner, dict) or part not in inner:
            return
        slot = _ValueSlot(inner, part)
    yield slot


def _objects(record: dict[str, RecordValue]) -> list[dict[str, RecordValue]]:
    objects = record.get("objects")
    if not isinstance(objects, list):
        return []
    return [entry for entry in objects if isinstance(entry, dict)]


_DEFAULT_PORTS = {"http": 80, "https": 443}


@dataclass(frozen=True, slots=True)
class _Address:
    """Where an origin is, and the placeholders for it, its name and port."""

    origin: str
    authority: str
    host: str
    placeholders: tuple[str, str, str]

    def hidden(self, text: str) -> str:
        """Put the origin, its name and port in a text behind placeholders, but not a longer name that ends in its
        name, such as that of another origin of its site, or a longer port."""
        for part, placeholder in zip((self.origin, self.authority, self.host), self.placeholders, strict=True):
            text = re.sub(rf"(?<![\w.-]){re.escape(part)}(?![\w-])", placeholder, text)
        return text


@dataclass(frozen=True, slots=True)
class _Addresses:
    """The top-level page's origin, the other origin of its site and the other site, at its scheme and port."""

    port: str | None
    origins: tuple[_Address, ...]

    @classmethod
    def of(cls, url: str) -> "_Addresses":
        parts = urlsplit(url)
        if not parts.scheme or not parts.hostname:
            raise ValueError(f"{url!r} has no origin")
        # A URL in a page leaves out its scheme's own port, as location.origin does.
        default = parts.port is not None and _DEFAULT_PORTS.get(parts.scheme) == parts.port
        port = None if parts.port is None or default else str(parts.port)

        def address(host: str, placeholders: tuple[str, str, str]) -> _Address:
            authority = host if port is None else f"{host}:{port}"
            return _Address(f"{parts.scheme}://{authority}", authority, host, placeholders)

        return cls(
            port,
            (
                address(parts.hostname, (ORIGIN_PLACEHOLDER, AUTHORITY_PLACEHOLDER, HOST_PLACEHOLDER)),
                address(
                    SAME_SITE_HOST,
                    (SAME_SITE_ORIGIN_PLACEHOLDER, SAME_SITE_AUTHORITY_PLACEHOLDER, SAME_SITE_HOST_PLACEHOLDER),
                ),
                address(
                    CROSS_SITE_HOST,
                    (CROSS_SITE_ORIGIN_PLACEHOLDER, CROSS_SITE_AUTHORITY_PLACEHOLDER, CROSS_SITE_HOST_PLACEHOLDER),
                ),
            ),
        )

    def placeholders(self, text: str) -> str:
        """Put each origin, name and port in a text behind its placeholders."""
        if text == self.port:
            return PORT_PLACEHOLDER
        for address in self.origins:
            text = address.hidden(text)
        return text


def _behind_placeholders(value: RecordValue, address: _Addresses) -> RecordValue:
    if isinstance(value, str):
        return address.placeholders(value)
    if isinstance(value, list):
        return [_behind_placeholders(item, address) for item in value]
    if isinstance(value, dict):
        return {key: _behind_placeholders(item, address) for key, item in value.items()}
    return value
