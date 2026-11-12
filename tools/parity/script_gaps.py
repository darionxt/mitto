"""The gaps of a Script Fingerprint: where a Page's Normal Form differs from a Baseline, the kind of each, and their list.

docs/parity.md, "The Script Fingerprint", says how a gap reads and how its kind is decided.
"""

import difflib
import json
from collections import Counter
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from enum import StrEnum
from typing import Any, Final, cast

from tools.capture.probe import RecordValue
from tools.capture.script_normal_form import HIGH_ENTROPY_VALUES, MACHINE_PLACES, Computed, Getter, Place
from tools.parity.gaps import KnownGapsError

__all__ = [
    "PERSONA_PLACES",
    "SCRIPT_KNOWN_GAPS_FILE_NAME",
    "GapKind",
    "ScriptDifference",
    "ScriptGap",
    "field_of",
    "loads_script_known_gaps",
    "place_kind",
    "same_json",
    "script_differences",
]

SCRIPT_KNOWN_GAPS_FILE_NAME: Final = "script_known_gaps.json"

type _Form = Mapping[str, RecordValue]
type _Entry = dict[str, RecordValue]


class GapKind(StrEnum):
    """What a gap of a Script Fingerprint is about, which says who closes it.

    A shape or a value is for mitto to match, and only missing behaviour stays a Known Gap at M7's exit.
    """

    SHAPE = "shape"
    """What a page sees of an object whatever it runs: its properties, their attributes, names, lengths and order, its
    prototype, and what the bindings' checks throw before a member's behaviour."""

    BEHAVIOUR = "behaviour"
    """What a member gives that its behaviour decides, which a Known Gap's default stands in for."""

    PERSONA = "persona"
    """A value that the Persona decides, such as the user agent."""

    MACHINE = "machine"
    """A value that the Machine decides, at a place that the Normal Form hides as <machine>."""


_INTL_RESOLVED_OPTIONS = (
    "new Intl.DateTimeFormat().resolvedOptions()",
    'new Intl.DateTimeFormat(undefined, {hour: "numeric"}).resolvedOptions()',
    "new Intl.NumberFormat().resolvedOptions()",
    "new Intl.Collator().resolvedOptions()",
    "new Intl.PluralRules().resolvedOptions()",
    "new Intl.RelativeTimeFormat().resolvedOptions()",
    "new Intl.ListFormat().resolvedOptions()",
    "new Intl.Segmenter().resolvedOptions()",
    'new Intl.DisplayNames(undefined, {type: "region"}).resolvedOptions()',
    "new Intl.DurationFormat().resolvedOptions()",
)

PERSONA_PLACES: Final[tuple[Place, ...]] = (
    *(Getter("navigator", name) for name in ("userAgent", "appVersion", "platform", "language", "languages")),
    *(Getter("navigator.userAgentData", name) for name in ("brands", "mobile", "platform")),
    Computed(HIGH_ENTROPY_VALUES),
    # What follows the locale, which comes from the Persona.
    *(Computed(key) for key in _INTL_RESOLVED_OPTIONS),
    Computed("Number: toLocaleString() of 1234567.891"),
)
"""Each place whose value the Persona decides, and with it each place within it, but those of MACHINE_PLACES, such as
the platform version among the high entropy values or the time zone among Intl's resolved options."""

_MACHINE_GETTERS = frozenset(place for place in MACHINE_PLACES if isinstance(place, Getter))
_PERSONA_GETTERS = frozenset(place for place in PERSONA_PLACES if isinstance(place, Getter))
_MACHINE_VALUES = tuple(place for place in MACHINE_PLACES if isinstance(place, Computed))
_PERSONA_VALUES = tuple(place for place in PERSONA_PLACES if isinstance(place, Computed))

_LISTS = ("own", "get", "calls")
_ORDER = "<order>"
_NOTHING = "(nothing)"
_ROOT = "globalThis"
_PROTOTYPE = "[[Prototype]]"
_DATA_TAGS = frozenset({"Array", "Object"})
_HEAD = ("kind", "tag", "proto")


@dataclass(frozen=True, slots=True)
class ScriptDifference:
    """One field in which two Normal Forms of a Script Fingerprint differ.

    Attributes:
        field: Where they differ, such as "Navigator.prototype own share" or 'values["..."]["timeZone"]'.
        kind: What the difference is about.
        baseline: The Baseline's value, or None if it has no such field.
        mitto: The Page's value, or None if it has no such field.
    """

    field: str
    kind: GapKind
    baseline: RecordValue
    mitto: RecordValue


@dataclass(frozen=True, slots=True)
class ScriptGap:
    """One field in which a Page's Script Fingerprint differs from a Baseline, as the known-gaps list holds it.

    Attributes:
        release: The Baseline's Chrome Release, such as "157".
        case: The Baseline's Recording Case, such as "page".
        kind: What the gap is about: "shape", "behaviour", "persona" or "machine".
        field: Where the two differ.
        baseline: The Baseline's value, or None if it has no such field.
        mitto: The Page's value, or None if it has no such field.
    """

    release: str
    case: str
    kind: GapKind
    field: str
    baseline: RecordValue
    mitto: RecordValue

    def __str__(self) -> str:
        return (
            f"Chrome {self.release}, {self.case}, {self.kind}, {self.field}\n"
            f"  Baseline: {_shown(self.baseline)}\n"
            f"  mitto:    {_shown(self.mitto)}"
        )


def _shown(value: RecordValue) -> str:
    return _NOTHING if value is None else json.dumps(value, ensure_ascii=False)


def same_json(baseline: RecordValue, mitto: RecordValue) -> bool:
    """Whether two parts of a record are the same JSON, where true is not 1 and false is not 0, as they are in Python."""
    if isinstance(baseline, bool) or isinstance(mitto, bool):
        return baseline is mitto
    if isinstance(baseline, list) and isinstance(mitto, list):
        return len(baseline) == len(mitto) and all(map(same_json, baseline, mitto))
    if isinstance(baseline, dict) and isinstance(mitto, dict):
        return baseline.keys() == mitto.keys() and all(same_json(value, mitto[key]) for key, value in baseline.items())
    return baseline == mitto


def field_of(place: Place) -> str:
    """Return the field of a gap at a place of a record, as script_differences() names it."""
    if isinstance(place, Getter):
        return f"{place.path} get {place.name}"
    return _value_field(place.key, place.keys)


def place_kind(place: Place) -> GapKind:
    """Return the kind of a gap in what a getter or the probe's third step gave at a place."""
    if isinstance(place, Getter):
        if place in _MACHINE_GETTERS:
            return GapKind.MACHINE
        return GapKind.PERSONA if place in _PERSONA_GETTERS else GapKind.BEHAVIOUR
    if any(_within(place, machine) for machine in _MACHINE_VALUES):
        return GapKind.MACHINE
    if any(_within(place, persona) for persona in _PERSONA_VALUES):
        return GapKind.PERSONA
    return GapKind.BEHAVIOUR


def _within(place: Computed, outer: Computed) -> bool:
    return place.key == outer.key and place.keys[: len(outer.keys)] == outer.keys


def script_differences(baseline: _Form, mitto: _Form) -> Iterator[ScriptDifference]:
    """Yield every field in which a Page's Normal Form differs from a Baseline, with its kind.

    Objects compare by their path: one that a side lacks is one difference, with its kind, tag and prototype. Within an
    object, each key compares whole but its lists, whose items compare by their key, with one more difference for the
    order of the items both have. The values of the third step compare key by key, into their objects. Each is in the
    order of the Baseline, then what only the Page has.
    """
    baseline_objects = _objects(baseline)
    mitto_objects = _objects(mitto)
    baseline_edges = _edges(baseline_objects.values())
    mitto_edges = _edges(mitto_objects.values())
    for path, entry in baseline_objects.items():
        other = mitto_objects.get(path)
        if other is None:
            yield ScriptDifference(path, _reach_kind(path, baseline_edges), _head(entry), None)
        else:
            yield from _object_differences(path, entry, other, mitto_edges)
    for path, entry in mitto_objects.items():
        if path not in baseline_objects:
            yield ScriptDifference(path, _reach_kind(path, mitto_edges), None, _head(entry))
    yield from _values_differences(_dict(baseline.get("values")), _dict(mitto.get("values")))
    for key in _keys(baseline, mitto):
        if key in ("objects", "values") or same_json(baseline.get(key), mitto.get(key)):
            continue
        kind = GapKind.BEHAVIOUR if key == "reports" else GapKind.SHAPE
        yield ScriptDifference(key, kind, baseline.get(key), mitto.get(key))


def _objects(form: _Form) -> dict[str, _Entry]:
    objects = form.get("objects")
    if not isinstance(objects, list):
        return {}
    found: dict[str, _Entry] = {}
    for entry in objects:
        if isinstance(entry, dict) and isinstance(path := entry.get("path"), str):
            found[path] = entry
    return found


def _dict(value: RecordValue) -> dict[str, RecordValue]:
    return value if isinstance(value, dict) else {}


def _keys(baseline: Mapping[str, Any], mitto: Mapping[str, Any]) -> list[str]:
    return [*baseline, *(key for key in mitto if key not in baseline)]


def _head(entry: _Entry) -> dict[str, RecordValue]:
    return {key: entry[key] for key in _HEAD if key in entry}


type _Edge = tuple[str, str, str]
"""How the walk reached an object first: the path of the object it was on, "own", "get" or "proto", and the key."""


def _edges(objects: Iterable[_Entry]) -> dict[str, _Edge]:
    """Return how the walk reached each object first, from the paths that the probe gives them."""
    edges: dict[str, _Edge] = {}
    for entry in objects:
        path = cast(str, entry["path"])
        proto = _reference(entry.get("proto"))
        if proto is not None and proto == f"{path}.{_PROTOTYPE}":
            edges[proto] = (path, "proto", _PROTOTYPE)
        for section in ("own", "get"):
            for item in _items(entry, section):
                key = str(item[0])
                children = _child_paths(path, key)
                for value in item[1:] if section == "get" else item[2:]:
                    reference = _reference(value)
                    if reference is not None and (
                        reference in children or reference.removeprefix("get ").removeprefix("set ") in children
                    ):
                        edges.setdefault(reference, (path, section, key))
    return edges


def _child_paths(path: str, key: str) -> frozenset[str]:
    # As the probe's childPath(): an identifier after a dot, and at the root on its own, any other key in brackets as
    # JSON writes it, and a symbol, "@@" and its description, in brackets as it is.
    in_brackets = (f"{path}[{json.dumps(key, ensure_ascii=False)}]", f"{path}[{key}]")
    return frozenset((key, *in_brackets) if path == _ROOT else (f"{path}.{key}", *in_brackets))


def _reference(value: RecordValue) -> str | None:
    if isinstance(value, list) and len(value) == 2 and value[0] == "object" and isinstance(value[1], str):
        return value[1]
    return None


def _items(entry: _Entry, section: str) -> list[list[RecordValue]]:
    items = entry.get(section)
    if not isinstance(items, list):
        return []
    return [item for item in items if isinstance(item, list) and item]


def _reach_kind(path: str, edges: Mapping[str, _Edge]) -> GapKind:
    """Return the kind of a gap in an object, or in what it holds as data: that of the nearest getter on the way the walk
    reached it, or a shape if the walk reached it through own properties and prototypes alone."""
    seen: set[str] = set()
    while path in edges and path not in seen:
        seen.add(path)
        parent, section, key = edges[path]
        if section == "get":
            return place_kind(Getter(parent, key))
        path = parent
    return GapKind.SHAPE


def _object_differences(
    path: str, baseline: _Entry, mitto: _Entry, edges: Mapping[str, _Edge]
) -> Iterator[ScriptDifference]:
    for key in _keys(baseline, mitto):
        if key in ("path", *_LISTS) or same_json(baseline.get(key), mitto.get(key)):
            continue
        yield ScriptDifference(f"{path} {key}", GapKind.SHAPE, baseline.get(key), mitto.get(key))
    holds_data = baseline.get("kind") == "instance" and baseline.get("tag") in _DATA_TAGS
    for section in _LISTS:
        if same_json(baseline.get(section), mitto.get(section)):
            continue
        baseline_items = _identified(_items(baseline, section))
        mitto_items = _identified(_items(mitto, section))

        def kind_of(name: str, section: str = section) -> GapKind:
            if section == "get":
                return place_kind(Getter(path, name))
            if section == "own" and baseline.get("kind") == "instance" and (holds_data or _is_index(name)):
                return _reach_kind(path, edges)
            return GapKind.SHAPE

        for name, item in baseline_items.items():
            other = mitto_items.get(name)
            if other is None or not same_json(other, item):
                yield ScriptDifference(
                    f"{path} {section} {name}",
                    kind_of(_key(name)),
                    _item_value(section, item),
                    _item_value(section, other),
                )
        for name, item in mitto_items.items():
            if name not in baseline_items:
                yield ScriptDifference(
                    f"{path} {section} {name}", kind_of(_key(name)), None, _item_value(section, item)
                )
        order = _moved(list(baseline_items), list(mitto_items))
        if order is not None:
            kind = _reach_kind(path, edges) if section == "own" and holds_data else GapKind.SHAPE
            yield ScriptDifference(f"{path} {section} {_ORDER}", kind, *order)


def _identified(items: list[list[RecordValue]]) -> dict[str, list[RecordValue]]:
    """Return a list's items by their key, the later ones of a key that comes again numbered from 2, as "key#2"."""
    found: dict[str, list[RecordValue]] = {}
    counts = Counter[str]()
    for item in items:
        key = str(item[0])
        counts[key] += 1
        found[key if counts[key] == 1 else f"{key}#{counts[key]}"] = item
    return found


def _key(name: str) -> str:
    key, _, number = name.rpartition("#")
    return key if key and number.isdigit() else name


def _item_value(section: str, item: list[RecordValue] | None) -> RecordValue:
    if item is None:
        return None
    if section == "own" or len(item) != 2:
        return item[1:]
    return item[1]


def _is_index(key: str) -> bool:
    return key.isdigit() and (key == "0" or not key.startswith("0"))


def _moved(baseline: list[str], mitto: list[str]) -> tuple[RecordValue, RecordValue] | None:
    """Return the keys that the two sides have in another order, each with its place among the keys both have, or None
    if those are in the same order.

    The keys that keep their order are a longest common run that difflib finds, so that a key that moved is the only
    one that shows, however long the list.
    """
    shared = set(baseline) & set(mitto)
    baseline_shared = [key for key in baseline if key in shared]
    mitto_shared = [key for key in mitto if key in shared]
    if baseline_shared == mitto_shared:
        return None
    matcher = difflib.SequenceMatcher(a=baseline_shared, b=mitto_shared, autojunk=False)
    kept = {key for block in matcher.get_matching_blocks() for key in baseline_shared[block.a : block.a + block.size]}
    return (
        [[key, place] for place, key in enumerate(baseline_shared) if key not in kept],
        [[key, place] for place, key in enumerate(mitto_shared) if key not in kept],
    )


def _values_differences(baseline: dict[str, RecordValue], mitto: dict[str, RecordValue]) -> Iterator[ScriptDifference]:
    for key in _keys(baseline, mitto):
        baseline_entry, mitto_entry = baseline.get(key), mitto.get(key)
        if same_json(baseline_entry, mitto_entry):
            continue
        if _is_value(baseline_entry) and _is_value(mitto_entry):
            yield from _value_differences(key, (), _dict(baseline_entry)["value"], _dict(mitto_entry)["value"])
        else:
            yield ScriptDifference(_value_field(key, ()), place_kind(Computed(key)), baseline_entry, mitto_entry)


def _is_value(entry: RecordValue) -> bool:
    """Whether a value of the third step is only what it gave: {"value": ...}."""
    return isinstance(entry, dict) and list(entry) == ["value"]


def _value_differences(
    key: str, parts: tuple[str, ...], baseline: RecordValue, mitto: RecordValue
) -> Iterator[ScriptDifference]:
    if same_json(baseline, mitto):
        return
    if isinstance(baseline, dict) and isinstance(mitto, dict):
        for part in _keys(baseline, mitto):
            yield from _value_differences(key, (*parts, part), baseline.get(part), mitto.get(part))
        return
    yield ScriptDifference(_value_field(key, parts), place_kind(Computed(key, parts)), baseline, mitto)


def _value_field(key: str, parts: tuple[str, ...]) -> str:
    return "".join((f"values[{json.dumps(key, ensure_ascii=False)}]", *(f"[{json.dumps(part)}]" for part in parts)))


def loads_script_known_gaps(text: str) -> list[ScriptGap]:
    """Read a known-gaps list of a Script Fingerprint, which tools.parity.gaps.dumps_known_gaps() writes.

    Raises:
        KnownGapsError: The text is not such a list.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        gaps: list[ScriptGap] = []
        for entry in cast(list[dict[str, Any]], entries):
            gap = ScriptGap(**entry)
            gaps.append(ScriptGap(**{**entry, "kind": GapKind(gap.kind)}))
        return gaps
    except (ValueError, TypeError) as error:
        raise KnownGapsError(f"not a known-gaps list of a Script Fingerprint: {error}") from error
