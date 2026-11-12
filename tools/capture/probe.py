"""The probe, the script that records a page's Script Fingerprint, and what reads its record.

The probe is `probe.js` next to this module. It runs unchanged in real Chrome, for a Baseline, and in a Page, for a
Parity Check: an indirect eval of it gives a promise of its record as JSON's text. docs/runtime.md, "The probe", says
what it records and what it leaves out.
"""

import json
from collections.abc import Iterable
from pathlib import Path
from typing import Final, cast

type RecordValue = bool | int | float | str | list[RecordValue] | dict[str, RecordValue] | None
"""A record, or a part of one, as Python's json reads it."""

PROBE_PATH: Final = Path(__file__).with_name("probe.js")
"""Where the probe is."""

RUN_TO_RUN_VALUES: Final[tuple[tuple[str, str | None], ...]] = (
    ("performance", "timeOrigin"),
    ("performance.timing", None),
    ("document", "lastModified"),
    ("document.timeline", "currentTime"),
    ("navigation.currentEntry", "key"),
    ("navigation.currentEntry", "id"),
    ("navigator.connection", "rtt"),
    ("navigator.connection", "downlink"),
    ("performance.memory", None),
)
"""The getters whose values change from one run of a Chrome to the next: the path of the object that the probe reads
each on, and the getter's name, or None for each getter of that object.

#207 found all but the last over 11 runs of Chrome 157. document.lastModified is the time the page loaded, when its
response has no Last-Modified; the navigation entry's key and id are random; and rtt and downlink carry the renderer's
noise. performance.memory gives precise figures in a renderer that is locked to a site, as each is on the desktop, at most
every 50 ms, from a cache that console.memory's rounded figures share (WindowPerformance::memory() and HeapSizeCache in
Blink's core/timing). So what it gives depends on how long the walk took since it read console.memory."""

RUN_TO_RUN: Final = "<run>"
"""What without_run_to_run_values() writes in place of such a value."""


def probe_script() -> str:
    """Return the probe's source."""
    return PROBE_PATH.read_text(encoding="utf-8")


def read_record(text: str) -> dict[str, RecordValue]:
    """Read what the probe gave, and check that it is a whole record.

    Raises:
        ValueError: If the text is no record of the probe's format.
    """
    value: object = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("this is not a record of the probe")
    record = cast(dict[str, RecordValue], value)
    if record.get("format") != 1 or not isinstance(record.get("objects"), list):
        raise ValueError("this is not a record of the probe")
    return record


def without_run_to_run_values(record: dict[str, RecordValue]) -> dict[str, RecordValue]:
    """Return a copy of a record with each value in RUN_TO_RUN_VALUES as RUN_TO_RUN, so that two runs compare equal."""
    copy = json.loads(json.dumps(record))
    for entry in _objects(copy):
        hidden = {name for path, name in RUN_TO_RUN_VALUES if path == entry.get("path")}
        getters = entry.get("get")
        if not hidden or not isinstance(getters, list):
            continue
        for item in getters:
            if isinstance(item, list) and len(item) == 2 and (None in hidden or item[0] in hidden):
                item[1] = RUN_TO_RUN
    return copy


def dumps_record(record: dict[str, RecordValue]) -> str:
    """Write a record as JSON's text with each own property, getter's value and check on a line of its own, so that two
    records of one Chrome Release, or of two, differ by lines.

    A record of Chrome 157 takes about 1.3 MB this way, within the 1.5 MB a Baseline may take for each Chrome Release
    (docs/runtime.md, "The probe").
    """
    lines = ["{"]
    keys = list(record)
    for index, key in enumerate(keys):
        comma = "," if index < len(keys) - 1 else ""
        value = record[key]
        if key == "objects" and isinstance(value, list):
            lines.append(f"{_text(key)}: [")
            lines.extend(_object_lines(_objects(record)))
            lines.append(f"]{comma}")
        else:
            lines.append(f"{_text(key)}: {_text(value)}{comma}")
    lines.append("}")
    return "\n".join(lines) + "\n"


_LISTS = ("own", "get", "calls")


def _objects(record: dict[str, RecordValue]) -> list[dict[str, RecordValue]]:
    objects = record.get("objects")
    if not isinstance(objects, list):
        return []
    return [entry for entry in objects if isinstance(entry, dict)]


def _object_lines(objects: list[dict[str, RecordValue]]) -> Iterable[str]:
    for index, entry in enumerate(objects):
        head = {key: value for key, value in entry.items() if key not in _LISTS}
        lists = [key for key in entry if key in _LISTS]
        if not lists:
            yield _text(head) + ("," if index < len(objects) - 1 else "")
            continue
        yield _text(head)[:-1] + ","
        for list_index, key in enumerate(lists):
            yield f" {_text(key)}: ["
            items = entry[key]
            assert isinstance(items, list)
            for item_index, item in enumerate(items):
                yield "  " + _text(item) + ("," if item_index < len(items) - 1 else "")
            yield " ]" + ("," if list_index < len(lists) - 1 else "")
        yield "}" + ("," if index < len(objects) - 1 else "")


def _text(value: RecordValue) -> str:
    return json.dumps(value, ensure_ascii=False)
