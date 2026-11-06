"""Gaps: where a recording differs from a Baseline, and the list of those that are known."""

import json
from collections import Counter
from collections.abc import Iterable, Iterator
from dataclasses import asdict, dataclass
from typing import Any, cast

from tools.capture.names import GREASE
from tools.capture.recording import Json

KNOWN_GAPS_FILE_NAME = "known_gaps.json"

_ORDER = "<order>"
_NOTHING = "(nothing)"

type _Identified = dict[tuple[str, int], Json]
"""The items of a list by what identifies them: a name, and which item of that name it is, counted from 1."""


class KnownGapsError(Exception):
    """A known-gaps list cannot be read."""


@dataclass(frozen=True, slots=True)
class Gap:
    """One field in which a recording differs from a Baseline.

    Attributes:
        release: The Baseline's Chrome Release, such as "157".
        case: The Baseline's Recording Case, such as "navigation" or "fetch_post_text_cross_site".
        protocol: The Baseline's protocol, such as "h2".
        field: Where the two differ, such as "tls.cipher_suites" or "request.headers.user-agent".
        baseline: The Baseline's value, or None if it has no such field.
        mitto: The recording's value, or None if it has no such field.
    """

    release: str
    case: str
    protocol: str
    field: str
    baseline: Json
    mitto: Json

    def __str__(self) -> str:
        return (
            f"Chrome {self.release}, {self.case}, {self.protocol}, {self.field}\n"
            f"  Baseline: {_shown(self.baseline)}\n"
            f"  mitto:    {_shown(self.mitto)}"
        )


def differences(baseline: Json, mitto: Json, field: str = "") -> Iterator[tuple[str, Json, Json]]:
    """Yield the (field, Baseline's value, mitto's value) of every place where two parts of a normal form differ.

    None stands for the value of a field that one side does not have. There is at least one difference exactly when the
    two are not equal. This looks only at the shape of the data, so it needs no change when the normal form gets new
    fields:

    - Objects compare key by key.
    - Items with an identity compare with the item of the same identity, so that one missing item is one difference and
      not a shift of all the items after it. That covers lists of objects with a "type", such as extensions and frames,
      and lists of "name: value" lines, such as headers and settings. If the items they have in common are in another
      order, that is one more difference, in the field "<order>".
    - Everything else compares whole, including lists of plain values, such as cipher suites.
    """
    if baseline == mitto:
        return
    if isinstance(baseline, dict) and isinstance(mitto, dict):
        yield from _differences_by_key(baseline, mitto, field)
        return
    if isinstance(baseline, list) and isinstance(mitto, list):
        identified_baseline = _identified(baseline)
        identified_mitto = _identified(mitto)
        if identified_baseline is not None and identified_mitto is not None:
            yield from _differences_by_identity(identified_baseline, identified_mitto, field)
            return
    yield field, baseline, mitto


def _differences_by_key(
    baseline: dict[str, Json], mitto: dict[str, Json], field: str
) -> Iterator[tuple[str, Json, Json]]:
    for key, value in baseline.items():
        if key in mitto:
            yield from differences(value, mitto[key], _child(field, key))
        else:
            yield _child(field, key), value, None
    for key, value in mitto.items():
        if key not in baseline:
            yield _child(field, key), None, value


def _differences_by_identity(baseline: _Identified, mitto: _Identified, field: str) -> Iterator[tuple[str, Json, Json]]:
    for identity, value in baseline.items():
        name = _identity_name(identity)
        if identity in mitto:
            yield from differences(value, mitto[identity], _child(field, name))
        else:
            yield _child(field, name), value, None
    for identity, value in mitto.items():
        if identity not in baseline:
            yield _child(field, _identity_name(identity)), None, value
    shared_in_baseline = [_identity_name(identity) for identity in baseline if identity in mitto]
    shared_in_mitto = [_identity_name(identity) for identity in mitto if identity in baseline]
    if shared_in_baseline != shared_in_mitto:
        yield _child(field, _ORDER), list[Json](shared_in_baseline), list[Json](shared_in_mitto)


def _identified(items: list[Json]) -> _Identified | None:
    """Return a list's items by identity, or None if some item has no identity."""
    identified: _Identified = {}
    counts = Counter[str]()
    for item in items:
        name = _item_name(item)
        if name is None:
            return None
        counts[name] += 1
        identified[name, counts[name]] = item
    return identified


def _item_name(item: Json) -> str | None:
    if isinstance(item, dict):
        name = item.get("type")
        return name if isinstance(name, str) else None
    if isinstance(item, str):
        name, separator, _ = item.partition(": ")
        return name if separator or item == GREASE else None
    return None


def _identity_name(identity: tuple[str, int]) -> str:
    name, number = identity
    return name if number == 1 else f"{name}#{number}"


def _child(field: str, name: str) -> str:
    return f"{field}.{name}" if field else name


def _shown(value: Json) -> str:
    return _NOTHING if value is None else json.dumps(value)


def dumps_known_gaps(gaps: Iterable[Gap]) -> str:
    """Write a known-gaps list as text: one gap per line, so that a gap that closes is one line that goes."""
    lines = [json.dumps(asdict(gap)) for gap in gaps]
    return "[\n" + ",\n".join(lines) + "\n]\n" if lines else "[]\n"


def loads_known_gaps(text: str) -> list[Gap]:
    """Read a known-gaps list.

    Raises:
        KnownGapsError: The text is not a known-gaps list.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        return [Gap(**entry) for entry in cast(list[dict[str, Any]], entries)]
    except (ValueError, TypeError) as error:
        raise KnownGapsError(f"not a known-gaps list: {error}") from error


@dataclass(frozen=True, slots=True)
class Mismatches:
    """How the gaps a run found differ from the known-gaps list. It is false when they agree.

    Attributes:
        unlisted: The gaps that were found but are not in the list.
        gone: The gaps that are in the list but were not found.
    """

    unlisted: list[Gap]
    gone: list[Gap]

    def __bool__(self) -> bool:
        return bool(self.unlisted or self.gone)

    def __str__(self) -> str:
        sections: list[str] = []
        if self.unlisted:
            sections.append(f"Not in the known-gaps list ({len(self.unlisted)}):")
            sections += [str(gap) for gap in self.unlisted]
        if self.gone:
            sections.append(f"In the known-gaps list, but no longer there ({len(self.gone)}):")
            sections += [str(gap) for gap in self.gone]
        return "\n".join(sections)


def mismatches(found: Iterable[Gap], listed: Iterable[Gap]) -> Mismatches:
    """Compare the gaps a run found with the known-gaps list. A gap whose value has changed is a different gap."""
    found_by_text = {_identity_text(gap): gap for gap in found}
    listed_by_text = {_identity_text(gap): gap for gap in listed}
    return Mismatches(
        unlisted=[gap for text, gap in found_by_text.items() if text not in listed_by_text],
        gone=[gap for text, gap in listed_by_text.items() if text not in found_by_text],
    )


def _identity_text(gap: Gap) -> str:
    return json.dumps(asdict(gap), sort_keys=True)
