"""What the Baseline of a later Recording Case of a Script Fingerprint keeps: only what differs from the first case's.

#210 chose that a Recording Case after the first, the top-level page, keeps only what differs from the first in its
Baseline. script_delta() writes that from the two Normal Forms of one recording session, and with_script_delta() gives
the later case's Normal Form back from the first's, which a Parity Check compares a Page's with. docs/capture.md, "What a
later case keeps", says how one reads.

A delta holds:

    format         The record's format.
    differs from   The first case, whose Normal Form it differs from.
    objects        For each object that differs, in the order the later case found them: its whole entry if the first
                   case lacks it, and otherwise its path with each key whose value differs. In a list, own, get or
                   calls, only the items that differ or are new. Its gone names what the later case lacks of it, a key
                   such as "calls" or an item such as "get top", and "own <order>" is the order of the later case's own
                   keys, where the order is not the first case's with the new keys at its end.
    gone           The path of each object that only the first case has.
    values         Each value of the probe's third step that differs or is new, whole, and null for one that the later
                   case lacks.
    reports        What the later case reported, where it differs, and so for any other key of a record.

A later case lists its objects in its own order, those that the first case has too in the first's, and the new ones
after them: no comparison reads the order of objects, which follows their own properties.
"""

import json
from collections.abc import Iterable, Iterator
from typing import cast

from tools.capture.probe import RecordValue

_LISTS = ("own", "get", "calls")
_ORDER = " <order>"
_GONE = "gone"
_DIFFERS_FROM = "differs from"
_OBJECTS = "objects"
_VALUES = "values"
_FORMAT = "format"
_LINED = frozenset({*_LISTS, *(f"{section}{_ORDER}" for section in _LISTS), _GONE})

type _Entry = dict[str, RecordValue]


def script_delta(
    first: dict[str, RecordValue], form: dict[str, RecordValue], first_case: str
) -> dict[str, RecordValue]:
    """Return what of a later Recording Case's Normal Form differs from the first case's, as a delta.

    Args:
        first: The first case's Normal Form, from the same recording session.
        form: The later case's Normal Form.
        first_case: The first case's name, such as "page".

    Raises:
        ValueError: If a list of an object holds one key twice, which the probe never gives.
    """
    first_objects = _objects(first)
    form_objects = _objects(form)
    objects: list[RecordValue] = []
    for path, entry in form_objects.items():
        other = first_objects.get(path)
        change = entry if other is None else _entry_change(other, entry)
        if change is not None:
            objects.append(change)
    delta: dict[str, RecordValue] = {_FORMAT: form.get(_FORMAT), _DIFFERS_FROM: first_case, _OBJECTS: objects}
    delta[_GONE] = [path for path in first_objects if path not in form_objects]
    values = _dict(first.get(_VALUES))
    form_values = _dict(form.get(_VALUES))
    delta[_VALUES] = {
        **{key: entry for key, entry in form_values.items() if key not in values or not _same(values[key], entry)},
        **{key: None for key in values if key not in form_values},
    }
    for key in _keys(first, form):
        if key in (_FORMAT, _OBJECTS, _VALUES) or (key in first and key in form and _same(first[key], form[key])):
            continue
        delta[key] = form.get(key)
    return delta


def with_script_delta(first: dict[str, RecordValue], delta: dict[str, RecordValue]) -> dict[str, RecordValue]:
    """Return the Normal Form of a later Recording Case, from the first case's and what script_delta() gave.

    Raises:
        ValueError: If the delta names something that the first case's Normal Form lacks, so that it is a delta of
            another.
    """
    objects = _objects(_copy(first))
    for path in _strings(delta.get(_GONE)):
        if objects.pop(path, None) is None:
            raise ValueError(f"the first case has no object at {path}")
    for change in _entries(_copy(delta).get(_OBJECTS)):
        path = cast(str, change["path"])
        entry = objects.get(path)
        objects[path] = change if entry is None else _changed_entry(entry, change)
    form = _copy(first)
    form[_FORMAT] = delta.get(_FORMAT)
    form[_OBJECTS] = cast(list[RecordValue], list(objects.values()))
    values = _dict(form.get(_VALUES))
    for key, entry in _dict(delta.get(_VALUES)).items():
        if entry is not None:
            values[key] = entry
        elif values.pop(key, None) is None:
            raise ValueError(f"the first case has no value {key!r}")
    form[_VALUES] = values
    for key, value in delta.items():
        if key in (_FORMAT, _DIFFERS_FROM, _OBJECTS, _GONE, _VALUES):
            continue
        if value is None:
            form.pop(key, None)
        else:
            form[key] = value
    return form


def read_script_delta(text: str) -> dict[str, RecordValue]:
    """Read what dumps_script_delta() wrote, and check that it is a delta.

    Raises:
        ValueError: If the text is no delta of a Normal Form.
    """
    value: object = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("this is not a delta of a Script Fingerprint")
    delta = cast(dict[str, RecordValue], value)
    if (
        delta.get(_FORMAT) != 1
        or not isinstance(delta.get(_DIFFERS_FROM), str)
        or not isinstance(delta.get(_OBJECTS), list)
        or not isinstance(delta.get(_GONE), list)
        or not isinstance(delta.get(_VALUES), dict)
    ):
        raise ValueError("this is not a delta of a Script Fingerprint")
    return delta


def differs_from(delta: dict[str, RecordValue]) -> str:
    """Return the first case that a delta differs from, such as "page"."""
    return cast(str, delta[_DIFFERS_FROM])


def dumps_script_delta(delta: dict[str, RecordValue]) -> str:
    """Write a delta as JSON's text with each item of its lists on a line of its own, as dumps_record() writes a record,
    so that two deltas differ by lines."""
    lines = ["{"]
    keys = list(delta)
    for index, key in enumerate(keys):
        comma = "," if index < len(keys) - 1 else ""
        value = delta[key]
        if key == _OBJECTS and isinstance(value, list):
            lines.append(f"{_text(key)}: [")
            entries = _entries(value)
            for entry_index, entry in enumerate(entries):
                lines.extend(_entry_lines(entry, last=entry_index == len(entries) - 1))
            lines.append(f"]{comma}")
        elif key == _GONE and isinstance(value, list):
            lines.append(f"{_text(key)}: [")
            lines.extend(_items(value))
            lines.append(f"]{comma}")
        elif key == _VALUES and isinstance(value, dict) and value:
            lines.append(f"{_text(key)}: {{")
            names = list(value)
            lines.extend(
                f"{_text(name)}: {_text(value[name])}{',' if name_index < len(names) - 1 else ''}"
                for name_index, name in enumerate(names)
            )
            lines.append(f"}}{comma}")
        else:
            lines.append(f"{_text(key)}: {_text(value)}{comma}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _entry_lines(entry: _Entry, *, last: bool) -> Iterator[str]:
    # As dumps_record() writes an object: what is no list on a line, then each item of each list on a line of its own.
    head = {key: value for key, value in entry.items() if key not in _LINED}
    lists = [key for key in entry if key in _LINED]
    if not lists:
        yield _text(head) + ("" if last else ",")
        return
    yield _text(head)[:-1] + ","
    for list_index, key in enumerate(lists):
        yield f" {_text(key)}: ["
        yield from ("  " + line for line in _items(cast(list[RecordValue], entry[key])))
        yield " ]" + ("," if list_index < len(lists) - 1 else "")
    yield "}" + ("" if last else ",")


def _items(items: list[RecordValue]) -> Iterator[str]:
    for index, item in enumerate(items):
        yield _text(item) + ("," if index < len(items) - 1 else "")


def _entry_change(first: _Entry, form: _Entry) -> _Entry | None:
    """Return what of an object's entry differs from the first case's, or None if nothing does."""
    change: _Entry = {"path": form["path"]}
    gone: list[RecordValue] = [key for key in first if key not in form]
    for key, value in form.items():
        if key == "path" or (key in first and _same(first[key], value)):
            continue
        if key not in _LISTS or key not in first:
            change[key] = value
            continue
        items, gone_items, order = _list_change(_list(first[key]), _list(value))
        change[key] = cast(list[RecordValue], items)
        gone += [f"{key} {item}" for item in gone_items]
        if order is not None:
            change[f"{key}{_ORDER}"] = cast(list[RecordValue], order)
    if gone:
        change[_GONE] = gone
    return change if len(change) > 1 else None


def _list_change(
    first: list[list[RecordValue]], form: list[list[RecordValue]]
) -> tuple[list[list[RecordValue]], list[str], list[str] | None]:
    """Return the items of a list that differ or are new, the keys that are gone, and the order of the later case's
    keys, or None where the first case's order, with the new keys at its end, is it."""
    first_items = _keyed(first)
    form_items = _keyed(form)
    items = [item for key, item in form_items.items() if key not in first_items or not _same(first_items[key], item)]
    gone = [key for key in first_items if key not in form_items]
    rebuilt = [key for key in first_items if key in form_items] + [key for key in form_items if key not in first_items]
    return items, gone, None if rebuilt == list(form_items) else list(form_items)


def _changed_entry(first: _Entry, change: _Entry) -> _Entry:
    entry = dict(first)
    gone = _strings(change.get(_GONE))
    for name in gone:
        if " " not in name and entry.pop(name, None) is None:
            raise ValueError(f"the first case's {first['path']} has no {name}")
    for key, value in change.items():
        if key in ("path", _GONE) or key.endswith(_ORDER):
            continue
        if key not in _LISTS or key not in entry:
            entry[key] = value
            continue
        prefix = f"{key} "
        items = _keyed(_list(entry[key]))
        for name in (name.removeprefix(prefix) for name in gone if name.startswith(prefix)):
            if items.pop(name, None) is None:
                raise ValueError(f"the first case's {first['path']} has no {prefix}{name}")
        for item in _list(value):
            items[str(item[0])] = item
        order = change.get(f"{key}{_ORDER}")
        if order is not None:
            items = {name: items[name] for name in _strings(order)}
        entry[key] = cast(list[RecordValue], list(items.values()))
    return entry


def _keyed(items: Iterable[list[RecordValue]]) -> dict[str, list[RecordValue]]:
    keyed: dict[str, list[RecordValue]] = {}
    for item in items:
        key = str(item[0])
        if key in keyed:
            raise ValueError(f"a list holds {key!r} twice")
        keyed[key] = item
    return keyed


def _same(first: RecordValue, form: RecordValue) -> bool:
    # As JSON writes them, where true is not 1, as it is in Python.
    return json.dumps(first) == json.dumps(form)


def _copy(record: dict[str, RecordValue]) -> dict[str, RecordValue]:
    return cast(dict[str, RecordValue], json.loads(json.dumps(record)))


def _objects(record: dict[str, RecordValue]) -> dict[str, _Entry]:
    return {cast(str, entry["path"]): entry for entry in _entries(record.get(_OBJECTS))}


def _entries(value: RecordValue) -> list[_Entry]:
    if not isinstance(value, list):
        return []
    return [entry for entry in value if isinstance(entry, dict) and isinstance(entry.get("path"), str)]


def _list(value: RecordValue) -> list[list[RecordValue]]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, list) and item]


def _strings(value: RecordValue) -> list[str]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str)]


def _dict(value: RecordValue) -> dict[str, RecordValue]:
    return dict(value) if isinstance(value, dict) else {}


def _keys(first: dict[str, RecordValue], form: dict[str, RecordValue]) -> list[str]:
    return [*first, *(key for key in form if key not in first)]


def _text(value: RecordValue) -> str:
    return json.dumps(value, ensure_ascii=False)
