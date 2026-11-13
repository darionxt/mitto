"""The form of each value that a normal form hides as <machine>, and a check that recordings keep to those forms.

A Parity Check cannot compare a value that the normal form hides, so its form is checked instead: how Chrome writes it,
which is how a Session or its Page writes it. Each form of a hint is what Chrome on Windows can send for it, by
docs/persona.md, "What decides each value", and each form of a Script Fingerprint's value what Chrome on Windows can
give a page, by its place in the probe's record. The tests hold a Session's values to these forms, and this module's
check holds real Chrome's. For a Script Fingerprint it also looks for each value of the machine in the record's Normal
Form, beside the place that hides it.

The check reads the values of the machine that recorded them, so it runs where the recordings are kept, and it prints
only the forms, how many values each had, whether they all had their form, and where a value of the machine shows:

    uv run python -m tools.capture.machine_forms recordings/a recordings/b
"""

import argparse
import json
import re
import sys
from collections import Counter
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from mitto._machine import windows_time_zones
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE, MACHINE_PLACEHOLDER
from tools.capture.probe import RecordValue
from tools.capture.script_normal_form import (
    DATE_STRINGS,
    DATE_TIME_FORMAT,
    DATE_TIME_FORMAT_HOURS,
    HIGH_ENTROPY_VALUES,
    MEDIA_AGREEMENT,
    MEDIA_INTEGERS,
    MEDIA_KEYWORDS,
    TIME_ZONE_OFFSETS,
    Computed,
    Getter,
    Place,
    machine_values,
    machine_values_shown,
    read_script_raw_record,
)
from tools.capture.script_page import SCRIPT_DIRECTORY, SCRIPT_RUNS, TOP_LEVEL_PAGE


@dataclass(frozen=True, slots=True)
class Form:
    """How the value of a hint is written.

    Attributes:
        description: The form in words. It says nothing of any one machine, so it can go anywhere.
        pattern: What a value of this form matches in full.
    """

    description: str
    pattern: re.Pattern[str]

    def fits(self, value: str) -> bool:
        """Return whether a value has this form."""
        return self.pattern.fullmatch(value) is not None


def _form(description: str, pattern: str) -> Form:
    return Form(description, re.compile(pattern))


_MEMORY = _form("2, 4, 8, 16 or 32", r"2|4|8|16|32")
_SCALE = _form("a number from 1 to below 10, in its shortest decimal, such as 1.25", r"[1-9](\.[0-9]*[1-9])?")
_SIZE = _form("a whole number from 1", r"[1-9][0-9]*")
_REDUCED = _form("no-preference or reduce", r"no-preference|reduce")

FORMS: Mapping[str, Form] = {
    "device-memory": _MEMORY,
    "sec-ch-device-memory": _MEMORY,
    "dpr": _SCALE,
    "sec-ch-dpr": _SCALE,
    "viewport-width": _SIZE,
    "sec-ch-viewport-width": _SIZE,
    "sec-ch-viewport-height": _SIZE,
    "rtt": _form("a multiple of 50 from 0 to 3000", r"0|50|[1-9][05]0|[12][0-9][05]0|3000"),
    "downlink": _form("a multiple of 0.05 from 0 to 10, in its shortest decimal", r"10|[0-9](\.([1-9]|[0-9]5))?"),
    "ect": _form("slow-2g, 2g, 3g or 4g", r"slow-2g|2g|3g|4g"),
    "sec-ch-prefers-color-scheme": _form("light or dark", r"light|dark"),
    "sec-ch-prefers-reduced-motion": _REDUCED,
    "sec-ch-prefers-reduced-transparency": _REDUCED,
    "sec-ch-ua-platform-version": _form(
        'the version of the UniversalApiContract of a Windows 10 or 11, in quotes, such as "19.0.0"',
        r'"(1|2|3|4|5|6|7|8|10|14|15|19)\.0\.0"',
    ),
}
"""The form of each hint that the normal form hides as <machine>, by lower-case name."""


@dataclass(frozen=True, slots=True)
class ValueForm:
    """How a value of a Script Fingerprint is written, as JSON's value in the probe's record.

    Attributes:
        description: The form in words. It says nothing of any one machine, so it can go anywhere.
        fits: Whether a value has this form.
    """

    description: str
    fits: Callable[[RecordValue], bool]


def _is_number(value: RecordValue) -> bool:
    return isinstance(value, int | float) and not isinstance(value, bool)


def _number_text(value: RecordValue) -> str:
    """Write a number as JavaScript does, in its shortest decimal."""
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return repr(value)


def _whole_numbers(description: str, least: int, most: int | None = None) -> ValueForm:
    def fits(value: RecordValue) -> bool:
        if not isinstance(value, int) or isinstance(value, bool):
            return False
        return least <= value and (most is None or value <= most)

    return ValueForm(description, fits)


def _numbers_like(form: Form) -> ValueForm:
    """The form of a number that a hint writes as text, as a page reads it."""
    return ValueForm(form.description, lambda value: _is_number(value) and form.fits(_number_text(value)))


def _one_of(description: str, *values: RecordValue) -> ValueForm:
    texts = frozenset(json.dumps(value) for value in values)
    return ValueForm(description, lambda value: json.dumps(value) in texts)


def _text(description: str, pattern: str) -> ValueForm:
    compiled = re.compile(pattern)
    return ValueForm(description, lambda value: isinstance(value, str) and compiled.fullmatch(value) is not None)


def _is_offset(value: RecordValue) -> bool:
    """Whether a value is what getTimezoneOffset() can give, in minutes."""
    if not isinstance(value, int) or isinstance(value, bool):
        return False
    return -840 <= value <= 720 and value % 15 == 0


def _offsets(value: RecordValue) -> bool:
    return isinstance(value, list) and len(value) == 12 and all(_is_offset(offset) for offset in value)


def _is_extent(value: RecordValue) -> bool:
    return isinstance(value, int | float) and not isinstance(value, bool) and value > 0


_DIP_SIZE = _whole_numbers("a whole number of device-independent pixels from 1", 1)
_DIP_PLACE = _whole_numbers("a whole number of device-independent pixels, also below 0", -(1 << 31))
_DIP_EXTENT = ValueForm("a number of device-independent pixels above 0", _is_extent)
_BOOLEAN = _one_of("true or false", True, False)
_HEAP_LIMIT = _whole_numbers(
    "a whole number of bytes from 128 MiB to 8 GiB, as V8 gives it or rounded to three digits", 1 << 27, 1 << 33
)
_REDUCED_KEYWORDS = _one_of('["no-preference"] or ["reduce"]', ["no-preference"], ["reduce"])
_WINTER_OR_SUMMER_DAY = r"(1|7)/1[56]/2026"
_TIME_OF_DAY = r"(1[0-2]|[1-9]):[0-5][0-9]:00[ \u202f](AM|PM)"
_ZONE_TIME = r"([01][0-9]|2[0-3]):[0-5][0-9]:00 GMT[+-][0-9]{4} \([^()]+\)"
_DATE_FORMS: Mapping[str, ValueForm] = {
    "toString": _text(
        "the day, as Date writes it, such as Thu Jan 15 2026 07:00:00 GMT-0500 (Eastern Standard Time)",
        rf"(Mon|Tue|Wed|Thu|Fri|Sat|Sun) (Jan|Jul) 1[56] 2026 {_ZONE_TIME}",
    ),
    "toTimeString": _text(
        "the time of day, as Date writes it, such as 07:00:00 GMT-0500 (Eastern Standard Time)", _ZONE_TIME
    ),
    "toLocaleString": _text(
        "the day and time as en-US writes them, such as 1/15/2026, 7:00:00 AM",
        rf"{_WINTER_OR_SUMMER_DAY}, {_TIME_OF_DAY}",
    ),
    "toLocaleDateString": _text("the day as en-US writes it, such as 1/15/2026", _WINTER_OR_SUMMER_DAY),
    "toLocaleTimeString": _text("the time as en-US writes it, such as 7:00:00 AM", _TIME_OF_DAY),
    "timeZoneName long": _text(
        "the day and the zone's long name as en-US writes them, such as 1/15/2026, Eastern Standard Time",
        rf"{_WINTER_OR_SUMMER_DAY}, [^,]+",
    ),
}


def _getter_forms(path: str, forms: Mapping[str, ValueForm]) -> dict[Place, ValueForm]:
    return {Getter(path, name): form for name, form in forms.items()}


def _part_forms(key: str, forms: Mapping[str, ValueForm]) -> dict[Place, ValueForm]:
    return {Computed(key, (name,)): form for name, form in forms.items()}


SCRIPT_FORMS: Mapping[Place, ValueForm] = {
    **_getter_forms(
        "globalThis",
        {
            "innerWidth": _DIP_SIZE,
            "innerHeight": _DIP_SIZE,
            "outerWidth": _DIP_SIZE,
            "outerHeight": _DIP_SIZE,
            "screenX": _DIP_PLACE,
            "screenY": _DIP_PLACE,
            "screenLeft": _DIP_PLACE,
            "screenTop": _DIP_PLACE,
            "devicePixelRatio": _numbers_like(_SCALE),
        },
    ),
    **_getter_forms(
        "screen",
        {
            "availWidth": _DIP_SIZE,
            "availHeight": _DIP_SIZE,
            "width": _DIP_SIZE,
            "height": _DIP_SIZE,
            "colorDepth": _one_of("24, or 30 on a display with HDR", 24, 30),
            "pixelDepth": _one_of("24, or 30 on a display with HDR", 24, 30),
            "availLeft": _DIP_PLACE,
            "availTop": _DIP_PLACE,
            "isExtended": _BOOLEAN,
        },
    ),
    **_getter_forms(
        "screen.orientation",
        {
            "type": _one_of(
                "landscape-primary, portrait-primary, landscape-secondary or portrait-secondary",
                "landscape-primary",
                "portrait-primary",
                "landscape-secondary",
                "portrait-secondary",
            ),
            "angle": _one_of("0, 90, 180 or 270", 0, 90, 180, 270),
        },
    ),
    **_getter_forms("visualViewport", {"width": _DIP_EXTENT, "height": _DIP_EXTENT}),
    **_getter_forms(
        'viewport.segments["0"]',
        {"width": _DIP_EXTENT, "height": _DIP_EXTENT, "right": _DIP_EXTENT, "bottom": _DIP_EXTENT},
    ),
    **_getter_forms(
        "navigator",
        {
            "hardwareConcurrency": _whole_numbers("a whole number from 1 to 64", 1, 64),
            "deviceMemory": _one_of("2, 4, 8, 16 or 32", 2, 4, 8, 16, 32),
            "maxTouchPoints": _whole_numbers("a whole number from 0 to 256", 0, 256),
            "cpuPerformance": _whole_numbers("a whole number from 0, for none known, to 4", 0, 4),
        },
    ),
    **_getter_forms(
        "navigator.connection",
        {
            "effectiveType": _one_of("slow-2g, 2g, 3g or 4g", "slow-2g", "2g", "3g", "4g"),
            "rtt": _numbers_like(FORMS["rtt"]),
            "downlink": _numbers_like(FORMS["downlink"]),
        },
    ),
    Getter("console.memory", "jsHeapSizeLimit"): _HEAP_LIMIT,
    Getter("performance.memory", "jsHeapSizeLimit"): _HEAP_LIMIT,
    Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)): _text(
        "the version of the UniversalApiContract of a Windows 10 or 11, such as 19.0.0",
        r"(1|2|3|4|5|6|7|8|10|14|15|19)\.0\.0",
    ),
    **_part_forms(
        MEDIA_KEYWORDS,
        {
            "display-state": _one_of(
                '[], where it is not parsed, or one of ["fullscreen"], ["normal"], ["minimized"] or ["maximized"]',
                [],
                ["fullscreen"],
                ["normal"],
                ["minimized"],
                ["maximized"],
            ),
            "orientation": _one_of('["portrait"] or ["landscape"]', ["portrait"], ["landscape"]),
            "pointer": _one_of('["none"], ["coarse"] or ["fine"]', ["none"], ["coarse"], ["fine"]),
            "any-pointer": _one_of(
                '["none"], ["coarse"], ["fine"] or ["coarse", "fine"]',
                ["none"],
                ["coarse"],
                ["fine"],
                ["coarse", "fine"],
            ),
            "hover": _one_of('["none"] or ["hover"]', ["none"], ["hover"]),
            "any-hover": _one_of('["none"], ["hover"] or ["none", "hover"]', ["none"], ["hover"], ["none", "hover"]),
            "color-gamut": _one_of(
                '["srgb"], ["srgb", "p3"] or ["srgb", "p3", "rec2020"]',
                ["srgb"],
                ["srgb", "p3"],
                ["srgb", "p3", "rec2020"],
            ),
            "inverted-colors": _one_of(
                '[], where it is not parsed, or ["inverted"] or ["none"]', [], ["inverted"], ["none"]
            ),
            "prefers-color-scheme": _one_of('["dark"] or ["light"]', ["dark"], ["light"]),
            "prefers-contrast": _one_of(
                '["no-preference"], ["more"], ["less"] or ["custom"]', ["no-preference"], ["more"], ["less"], ["custom"]
            ),
            "prefers-reduced-motion": _REDUCED_KEYWORDS,
            "dynamic-range": _one_of(
                '["standard"], or ["standard", "high"] with HDR', ["standard"], ["standard", "high"]
            ),
            "video-dynamic-range": _one_of(
                '[], where it is not parsed, or ["standard"], or ["standard", "high"] with HDR',
                [],
                ["standard"],
                ["standard", "high"],
            ),
            "prefers-reduced-transparency": _REDUCED_KEYWORDS,
            "forced-colors": _one_of('["none"] or ["active"]', ["none"], ["active"]),
        },
    ),
    Computed(MEDIA_INTEGERS, ("color",)): _one_of("[8], or [10] on a display with HDR", [8], [10]),
    **_part_forms(
        MEDIA_AGREEMENT,
        {
            "width = innerWidth": _BOOLEAN,
            "height = innerHeight": _BOOLEAN,
            "aspect-ratio = innerWidth / innerHeight": _BOOLEAN,
            "device-width = screen.width": _BOOLEAN,
            "device-height = screen.height": _BOOLEAN,
            "device-aspect-ratio = screen.width / screen.height": _BOOLEAN,
        },
    ),
    **{
        Computed(key, ("timeZone",)): ValueForm(
            "a time zone that Chrome on Windows gives, by its canonical name, such as America/New_York",
            lambda value: isinstance(value, str) and value in windows_time_zones(),
        )
        for key in (DATE_TIME_FORMAT, DATE_TIME_FORMAT_HOURS)
    },
    Computed(TIME_ZONE_OFFSETS): ValueForm(
        "12 whole numbers of minutes, each a multiple of 15 from -840 to 720", _offsets
    ),
    **{Computed(DATE_STRINGS, (day, name)): form for day in ("winter", "summer") for name, form in _DATE_FORMS.items()},
}
"""The form of each value that the Normal Form of a Script Fingerprint hides as <machine>, by its place in the probe's
record: what Chrome on Windows can give a page there, with the Persona's en-US locale."""

_RAW_RECORDINGS = "*.raw.json"


@dataclass(frozen=True, slots=True)
class HintCount:
    """How the values of one hint kept to its form.

    Attributes:
        name: The hint, by lower-case name.
        values: How many values it had.
        misfits: How many of those did not have its form.
    """

    name: str
    values: int
    misfits: int


def hidden_values(raw: Mapping[str, Any]) -> Iterator[tuple[str, str]]:
    """Yield the lower-case name and the value of each line that the normal form hides, from a raw recording.

    Args:
        raw: A recording in the raw form that the capture kit writes, as JSON data.
    """
    for connection in raw["connections"]:
        for request in connection["requests"]:
            for line in request["headers"]:
                name, _, value = str(line).partition(": ")
                if name.lower() in HINTS_FROM_THE_MACHINE:
                    yield name.lower(), value


def count_forms(values: Iterable[tuple[str, str]]) -> list[HintCount]:
    """Count the values of each hint, and those without its form, in the order of the hints' names.

    Args:
        values: The lower-case name and the value of each line, as hidden_values() gives them.
    """
    seen: Counter[str] = Counter()
    misfits: Counter[str] = Counter()
    for name, value in values:
        seen[name] += 1
        if not FORMS[name].fits(value):
            misfits[name] += 1
    return [HintCount(name, seen[name], misfits[name]) for name in sorted(FORMS)]


def check_directory(directory: Path) -> tuple[int, list[HintCount]]:
    """Count the forms in every raw recording of the wire under a directory, such as one that record.ps1 wrote.

    Returns:
        How many raw recordings there are, and the count of each hint over all of them.
    """
    paths = [path for path in sorted(directory.rglob(_RAW_RECORDINGS)) if not _is_script_record(directory, path)]
    values = (value for path in paths for value in hidden_values(json.loads(path.read_text(encoding="utf-8"))))
    return len(paths), count_forms(values)


@dataclass(frozen=True, slots=True)
class PlaceCount:
    """How the values at one place of a Script Fingerprint's record kept to its form.

    Attributes:
        place: The place.
        values: How many values it had.
        misfits: How many of those did not have its form.
    """

    place: Place
    values: int
    misfits: int

    @property
    def name(self) -> str:
        return str(self.place)


def count_script_forms(
    values: Iterable[tuple[Place, RecordValue]], places: Iterable[Place] = tuple(SCRIPT_FORMS)
) -> list[PlaceCount]:
    """Count the values at each place, and those without its form, in the order of the places.

    Args:
        values: Each place and its value, as machine_values() gives them.
        places: The places to count, those of the machine where the probe ran.
    """
    seen: Counter[Place] = Counter()
    misfits: Counter[Place] = Counter()
    for place, value in values:
        seen[place] += 1
        if not SCRIPT_FORMS[place].fits(value):
            misfits[place] += 1
    return [PlaceCount(place, seen[place], misfits[place]) for place in places]


@dataclass(frozen=True, slots=True)
class ScriptRecordCheck:
    """What the check found in one raw record of a Script Fingerprint.

    Attributes:
        raw_name: The raw record's path, from the directory.
        form_name: The path of its Normal Form, from the directory.
        counts: The count of each place.
        sought: How many values of the machine it looked for in the Normal Form, or None with no Normal Form there.
        shown: Where in the Normal Form one of them showed.
        own: For each place whose value the case decides, such as a frame's size, how many values it had and how many
            of those were not the case's own.
    """

    raw_name: str
    form_name: str
    counts: list[PlaceCount]
    sought: int | None
    shown: list[str]
    own: list[PlaceCount]

    @property
    def values(self) -> int:
        return sum(count.values for count in self.counts)

    @property
    def passed(self) -> bool:
        has_misfits = any(count.misfits for count in (*self.counts, *self.own))
        return self.values > 0 and not has_misfits and self.sought is not None and not self.shown


def check_script_records(directory: Path) -> list[ScriptRecordCheck]:
    """Check each raw record of a Script Fingerprint under a directory, and the file beside it that a Baseline is made of:
    its Normal Form, or for a later Recording Case what of it differs from the first case's.

    Each is held to the places of the machine where its probe ran, those of a top-level page for a record whose run is
    not known."""
    runs = {run.name: run for run in SCRIPT_RUNS}
    checks: list[ScriptRecordCheck] = []
    for path in sorted(directory.rglob(_RAW_RECORDINGS)):
        if not _is_script_record(directory, path):
            continue
        raw = read_script_raw_record(path.read_text(encoding="utf-8")).record
        name = path.relative_to(directory).as_posix().removesuffix(_RAW_RECORDINGS[1:])
        run = runs.get(name, TOP_LEVEL_PAGE)
        form_path = path.with_name(path.name.removesuffix(_RAW_RECORDINGS[1:]) + ".json")
        sought: int | None = None
        shown: list[str] = []
        if form_path.is_file():
            form = json.loads(form_path.read_text(encoding="utf-8"))
            sought, shown = machine_values_shown(raw, form, run.machine_places, run.own_values)
        checks.append(
            ScriptRecordCheck(
                path.relative_to(directory).as_posix(),
                form_path.relative_to(directory).as_posix(),
                count_script_forms(machine_values(raw, run.machine_places), run.machine_places),
                sought,
                shown,
                _count_own(raw, run.own_values),
            )
        )
    return checks


def _count_own(raw: dict[str, RecordValue], own: Mapping[Place, RecordValue]) -> list[PlaceCount]:
    """Count the values at each place whose value the case decides, and those that are not the case's own."""
    counts: list[PlaceCount] = []
    for place, expected in own.items():
        found = [value for _, value in machine_values(raw, (place,))]
        misfits = sum(json.dumps(value) != json.dumps(expected) for value in found)
        counts.append(PlaceCount(place, len(found), misfits))
    return counts


def _is_script_record(directory: Path, path: Path) -> bool:
    return path.relative_to(directory).parts[0] == SCRIPT_DIRECTORY


def main(argv: Sequence[str] | None = None) -> int:
    """Check the directories that the command line names, print the forms and the outcome, and return the exit code."""
    parser = argparse.ArgumentParser(
        prog="python -m tools.capture.machine_forms",
        description=f"Check that each value a normal form hides as {MACHINE_PLACEHOLDER} has the form a Session's has.",
    )
    parser.add_argument("directories", nargs="+", type=Path, metavar="DIRECTORY", help="a directory of recordings")
    args = parser.parse_args(argv)
    passed = True
    for directory in args.directories:
        passed = _check_and_print(directory) and passed
    return 0 if passed else 1


def _check_and_print(directory: Path) -> bool:
    recordings, counts = check_directory(directory)
    script_checks = check_script_records(directory)
    values = sum(count.values for count in counts)
    wire_passed = not any(count.misfits for count in counts)
    passed = (values > 0 or bool(script_checks)) and wire_passed and all(check.passed for check in script_checks)
    line = f"{directory}: {'pass' if passed else 'FAIL'}, {values} values in {recordings} raw recordings"
    if script_checks:
        line += f", and {len(script_checks)} raw records of a Script Fingerprint"
    print(line)
    if recordings or not script_checks:
        for count in counts:
            print(f"  {_outcome(count)}  {count.name}: {FORMS[count.name].description}, {_amount(count)}")
    for check in script_checks:
        print(f"  {check.raw_name}: {'pass' if check.passed else 'FAIL'}, {check.values} values")
        for place_count in check.counts:
            description = SCRIPT_FORMS[place_count.place].description
            print(f"    {_outcome(place_count)}  {place_count.name}: {description}, {_amount(place_count)}")
        own = {run.name: run.own_values for run in SCRIPT_RUNS}.get(check.raw_name.removesuffix(".raw.json"), {})
        for place_count in check.own:
            expected = json.dumps(own.get(place_count.place))
            print(
                f"    {_outcome(place_count)}  {place_count.name}: the case's own, {expected}, {_amount(place_count)}"
            )
        if check.sought is None:
            print(f"    FAIL  no {check.form_name} to look in")
        elif check.shown:
            print(f"    FAIL  a value of the machine shows in {check.form_name}, at {', '.join(check.shown)}")
        else:
            print(f"    pass  none of {check.sought} values of the machine shows in {check.form_name}")
    return passed


def _outcome(count: HintCount | PlaceCount) -> str:
    if count.misfits:
        return "FAIL"
    return "pass" if count.values else "none"


def _amount(count: HintCount | PlaceCount) -> str:
    if count.misfits:
        return f"{count.misfits} of {count.values} values without it"
    return f"{count.values} values"


if __name__ == "__main__":
    sys.exit(main())
