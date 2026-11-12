"""The probe, the script that records a page's Script Fingerprint, and what reads and writes its record.

The probe is `probe.js` next to this module. It runs unchanged in real Chrome, for a Baseline, and in a Page, for a
Parity Check: an indirect eval of it gives a promise of its record as JSON's text. docs/runtime.md, "The probe", says
what it records and what it leaves out, and script_normal_form.py writes its Normal Form.
"""

import json
from collections.abc import Iterable
from pathlib import Path
from typing import Final, cast

type RecordValue = bool | int | float | str | list[RecordValue] | dict[str, RecordValue] | None
"""A record, or a part of one, as Python's json reads it."""

PROBE_PATH: Final = Path(__file__).with_name("probe.js")
"""Where the probe is."""


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


def dumps_record(record: dict[str, RecordValue]) -> str:
    """Write a record as JSON's text with each own property, getter's value, check and value of the third step on a
    line of its own, so that two records of one Chrome Release, or of two, differ by lines.

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
        elif key == "values" and isinstance(value, dict) and value:
            lines.append(f"{_text(key)}: {{")
            names = list(value)
            lines.extend(
                f"{_text(name)}: {_text(value[name])}{',' if index < len(names) - 1 else ''}"
                for index, name in enumerate(names)
            )
            lines.append(f"}}{comma}")
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
