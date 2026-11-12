"""Tests of how the Parity Checks of a Script Fingerprint compare, name and sort a gap, and of their list.

They change the Baseline of Chrome 157 on purpose and need no native library.
"""

import copy
import json
from typing import Any

import pytest

from mitto import Machine
from mitto._persona import WINDOWS_PERSONAS
from tools.capture.probe import RecordValue
from tools.capture.script_normal_form import MACHINE_PLACES, Computed, Getter
from tools.parity import BASELINES_DIRECTORY, BaselineError, KnownGapsError, dumps_known_gaps, mismatches
from tools.parity.script import ScriptBaseline, ScriptRecording, find_script_gaps, load_script_baselines
from tools.parity.script_gaps import (
    PERSONA_PLACES,
    GapKind,
    ScriptDifference,
    ScriptGap,
    field_of,
    loads_script_known_gaps,
    same_json,
    script_differences,
)
from tools.parity.script_machine import NOISED, NOT_THE_MACHINES, PAGE_READS, THE_MACHINES, machine_misses

type Form = dict[str, Any]


@pytest.fixture(scope="module")
def chrome_157() -> Form:
    return next(baseline.form for baseline in load_script_baselines(BASELINES_DIRECTORY) if baseline.release == "157")


@pytest.fixture
def page(chrome_157: Form) -> Form:
    """A Page's Normal Form that is the Baseline's to begin with, for a test to change."""
    return copy.deepcopy(chrome_157)


def entry(form: Form, path: str) -> dict[str, Any]:
    return next(item for item in form["objects"] if item["path"] == path)


def item(form: Form, path: str, section: str, key: str) -> list[Any]:
    return next(each for each in entry(form, path)[section] if each[0] == key)


def differences(baseline: Form, page: Form) -> list[tuple[str, GapKind, RecordValue, RecordValue]]:
    return [(each.field, each.kind, each.baseline, each.mitto) for each in script_differences(baseline, page)]


def test_a_baseline_has_no_gap_with_itself(chrome_157: Form, page: Form) -> None:
    assert differences(chrome_157, page) == []


def test_each_release_of_the_supported_window_has_a_baseline_of_the_page() -> None:
    baselines = load_script_baselines(BASELINES_DIRECTORY)

    assert [(baseline.release, baseline.case) for baseline in baselines] == [
        (release, "page") for release in sorted(WINDOWS_PERSONAS)
    ]


@pytest.mark.parametrize(
    ("path", "section", "key", "value", "kind"),
    [
        ("Navigator.prototype", "own", "vendor", "-e-", GapKind.SHAPE),
        (
            "CookieStore.prototype",
            "calls",
            "set",
            ["rejects", "TypeError: Failed to execute 'set' on 'CookieStore': Illegal invocation"],
            GapKind.SHAPE,
        ),
        ("history", "get", "length", 0, GapKind.BEHAVIOUR),
        ("navigator", "get", "userAgent", "Mozilla/5.0", GapKind.PERSONA),
        ("navigator", "get", "hardwareConcurrency", ["throws", "TypeError: no"], GapKind.MACHINE),
    ],
    ids=["flags", "check", "getter", "persona", "machine"],
)
def test_a_changed_item_is_one_gap_of_its_kind(
    chrome_157: Form, page: Form, path: str, section: str, key: str, value: RecordValue, kind: GapKind
) -> None:
    item(page, path, section, key)[1] = value

    def shown(changed: list[Any]) -> RecordValue:
        return changed[1:] if section == "own" else changed[1]

    assert differences(chrome_157, page) == [
        (
            f"{path} {section} {key}",
            kind,
            shown(item(chrome_157, path, section, key)),
            shown(item(page, path, section, key)),
        )
    ]


def test_a_number_in_the_place_of_a_boolean_is_a_gap(chrome_157: Form, page: Form) -> None:
    item(page, "navigator", "get", "webdriver")[1] = 0
    key = 'new Intl.DateTimeFormat(undefined, {hour: "numeric"}).resolvedOptions()'
    page["values"][key]["value"]["hour12"] = 1

    assert differences(chrome_157, page) == [
        ("navigator get webdriver", GapKind.BEHAVIOUR, False, 0),
        (f'values[{json.dumps(key)}]["hour12"]', GapKind.PERSONA, True, 1),
    ]


@pytest.mark.parametrize(
    ("baseline", "mitto", "same"),
    [(False, 0, False), (True, 1, False), ([1, True], [1.0, True], True), ({"a": False}, {"a": 0}, False)],
)
def test_json_tells_a_boolean_from_a_number(baseline: RecordValue, mitto: RecordValue, same: bool) -> None:
    assert same_json(baseline, mitto) is same


def test_a_property_that_a_page_lacks_is_one_gap(chrome_157: Form, page: Form) -> None:
    entry(page, "Navigator.prototype")["own"].remove(item(page, "Navigator.prototype", "own", "vendor"))

    assert differences(chrome_157, page) == [
        ("Navigator.prototype own vendor", GapKind.SHAPE, ["ec", ["function", 0], None], None)
    ]


def test_a_key_of_an_object_compares_whole(chrome_157: Form, page: Form) -> None:
    entry(page, "Navigator.prototype")["tag"] = "Object"

    assert differences(chrome_157, page) == [("Navigator.prototype tag", GapKind.SHAPE, "Navigator", "Object")]


def test_an_object_that_a_page_lacks_is_one_gap_of_its_head(chrome_157: Form, page: Form) -> None:
    page["objects"].remove(entry(page, "Navigator.prototype"))

    assert differences(chrome_157, page) == [
        (
            "Navigator.prototype",
            GapKind.SHAPE,
            {"kind": "prototype", "tag": "Navigator", "proto": ["object", "Object.prototype"]},
            None,
        )
    ]


def test_an_object_that_a_getter_gives_is_behaviour_and_so_is_what_it_holds(chrome_157: Form, page: Form) -> None:
    # A Page whose navigation.currentEntry gives null lacks the object, and what the walk reached from it.
    item(page, "navigation", "get", "currentEntry")[1] = None
    page["objects"].remove(entry(page, "navigation.currentEntry"))

    kinds = {field: kind for field, kind, _, _ in differences(chrome_157, page)}

    assert kinds == {"navigation get currentEntry": GapKind.BEHAVIOUR, "navigation.currentEntry": GapKind.BEHAVIOUR}


def test_what_an_array_that_the_persona_decides_holds_is_the_personas(chrome_157: Form, page: Form) -> None:
    languages = entry(page, "navigator.languages")
    languages["own"] = [own for own in languages["own"] if own[0] != "0"]

    assert [(field, kind) for field, kind, _, _ in differences(chrome_157, page)] == [
        ("navigator.languages own 0", GapKind.PERSONA)
    ]


def test_an_array_index_of_a_platform_object_is_behaviour(chrome_157: Form, page: Form) -> None:
    scripts = entry(page, "document.scripts")
    scripts["own"] = [own for own in scripts["own"] if own[0] != "0"]

    assert [(field, kind) for field, kind, _, _ in differences(chrome_157, page)] == [
        ("document.scripts own 0", GapKind.BEHAVIOUR)
    ]


def test_a_key_that_moved_is_the_only_one_an_order_gap_names(chrome_157: Form, page: Form) -> None:
    own = entry(page, "globalThis")["own"]
    moved = own.pop(5)
    own.append(moved)
    shared = [each[0] for each in entry(chrome_157, "globalThis")["own"]]

    assert differences(chrome_157, page) == [
        ("globalThis own <order>", GapKind.SHAPE, [[moved[0], 5]], [[moved[0], len(shared) - 1]])
    ]


def test_the_values_of_the_third_step_compare_into_their_objects(chrome_157: Form, page: Form) -> None:
    values = page["values"]
    values["new Intl.NumberFormat().resolvedOptions()"]["value"]["locale"] = "en-GB"
    values["navigator.permissions.query(): the state of each"]["value"]["camera"] = {"value": "granted"}
    values["new Intl.DateTimeFormat().resolvedOptions()"]["value"]["timeZone"] = ["throws", "RangeError: no"]

    assert [(field, kind) for field, kind, _, _ in differences(chrome_157, page)] == [
        ('values["new Intl.DateTimeFormat().resolvedOptions()"]["timeZone"]', GapKind.MACHINE),
        ('values["new Intl.NumberFormat().resolvedOptions()"]["locale"]', GapKind.PERSONA),
        ('values["navigator.permissions.query(): the state of each"]["camera"]["value"]', GapKind.BEHAVIOUR),
    ]


def test_a_value_that_threw_compares_whole(chrome_157: Form, page: Form) -> None:
    key = "document.featurePolicy.features()"
    page["values"][key] = {"throws": "TypeError: no"}

    assert differences(chrome_157, page) == [
        (f"values[{json.dumps(key)}]", GapKind.BEHAVIOUR, chrome_157["values"][key], {"throws": "TypeError: no"})
    ]


def test_each_place_the_persona_decides_is_in_a_baseline(chrome_157: Form) -> None:
    getters = {(each["path"], getter[0]) for each in chrome_157["objects"] for getter in each.get("get", [])}

    for place in PERSONA_PLACES:
        if isinstance(place, Getter):
            assert (place.path, place.name) in getters
        else:
            assert place.key in chrome_157["values"]


def test_a_gap_at_a_place_has_the_field_that_the_comparison_names() -> None:
    assert field_of(Getter("navigator", "deviceMemory")) == "navigator get deviceMemory"
    assert field_of(Computed("Date: x", ("winter", "toString"))) == 'values["Date: x"]["winter"]["toString"]'


# The values of the machine.


def _record_of_the_machine(machine: Machine) -> dict[str, RecordValue]:
    """A record whose values at the places of PAGE_READS are the Machine's, as a Page should give them."""
    getters: dict[str, list[RecordValue]] = {}
    values: dict[str, Any] = {}
    for place in MACHINE_PLACES:
        read = PAGE_READS.get(place)
        if read is None:
            continue
        if isinstance(place, Getter):
            getters.setdefault(place.path, []).append([place.name, read(machine)])
        else:
            values.setdefault(place.key, {"value": {}})["value"][place.keys[0]] = read(machine)
    objects: list[RecordValue] = [
        {"path": path, "kind": "instance", "get": list(items)} for path, items in getters.items()
    ]
    return {"format": 1, "objects": objects, "values": values, "reports": []}


def _set(record: dict[str, RecordValue], place: Getter, value: RecordValue) -> None:
    objects = record["objects"]
    assert isinstance(objects, list)
    for each in objects:
        assert isinstance(each, dict)
        if each["path"] == place.path:
            getters = each["get"]
            assert isinstance(getters, list)
            for getter in getters:
                assert isinstance(getter, list)
                if getter[0] == place.name:
                    getter[1] = value


def test_a_page_that_gives_the_machines_values_has_no_gap_of_the_machine() -> None:
    machine = Machine.draw(seed=7)

    assert list(machine_misses(_record_of_the_machine(machine), machine)) == []


def test_a_value_that_is_not_the_machines_is_a_gap_that_holds_no_value() -> None:
    machine = Machine.draw(seed=7)
    record = _record_of_the_machine(machine)
    place = Getter("globalThis", "innerWidth")
    _set(record, place, machine.viewport_width + 1)

    assert list(machine_misses(record, machine)) == [(place, THE_MACHINES, NOT_THE_MACHINES)]


def test_a_value_with_noise_is_held_to_its_form_and_a_getter_that_threw_is_left_out() -> None:
    machine = Machine.draw(seed=7)
    record = _record_of_the_machine(machine)
    rtt, downlink = Getter("navigator.connection", "rtt"), Getter("navigator.connection", "downlink")
    assert {rtt, downlink} == NOISED
    _set(record, rtt, 3)
    _set(record, downlink, ["throws", "TypeError: no"])

    misses = list(machine_misses(record, machine))

    assert [(place, page) for place, _, page in misses] == [(rtt, "<machine>: not of that form")]
    assert misses[0][1].startswith("<machine>: ")


def test_false_is_no_zero() -> None:
    machine = Machine.draw(seed=7)
    record = _record_of_the_machine(machine)
    _set(record, Getter("screen", "isExtended"), 0)

    assert [place for place, _, _ in machine_misses(record, machine)] == [Getter("screen", "isExtended")]


# Running a check, with a recorder in the place of a Page.


def test_a_check_compares_the_normal_form_and_the_values_of_the_machine() -> None:
    machine = Machine.draw(seed=7)
    page = _record_of_the_machine(machine)
    page["objects"].append({"path": "Thing", "kind": "function"})  # type: ignore[union-attr]
    _set(page, Getter("navigator", "deviceMemory"), 3)
    baseline = ScriptBaseline("157", "page", {"format": 1, "objects": [], "values": {}, "reports": []})

    gaps = find_script_gaps([baseline], lambda _: ScriptRecording("https://capture.test/script/page", page, machine))

    assert ScriptGap("157", "page", GapKind.SHAPE, "Thing", None, {"kind": "function"}) in gaps
    assert (
        ScriptGap("157", "page", GapKind.MACHINE, "navigator get deviceMemory", THE_MACHINES, NOT_THE_MACHINES) in gaps
    )


def test_baselines_of_an_unknown_case_or_not_of_the_probe_are_refused(tmp_path: Any) -> None:
    (tmp_path / "157" / "script").mkdir(parents=True)
    (tmp_path / "157" / "script" / "frame.json").write_text('{"format": 1, "objects": []}')
    with pytest.raises(BaselineError, match="unknown Recording Case"):
        load_script_baselines(tmp_path)

    (tmp_path / "157" / "script" / "frame.json").rename(tmp_path / "157" / "script" / "page.json")
    (tmp_path / "157" / "script" / "page.json").write_text("[]")
    with pytest.raises(BaselineError, match="not a Normal Form"):
        load_script_baselines(tmp_path)


# The list.

GAP = ScriptGap("157", "page", GapKind.SHAPE, "Navigator.prototype own share", ["wec", ["function", 0]], None)
OTHER_GAP = ScriptGap("156", "page", GapKind.BEHAVIOUR, "history get length", 1, 0)


def test_the_list_reads_back_as_it_was_written_with_one_gap_per_line() -> None:
    text = dumps_known_gaps([GAP, OTHER_GAP])

    assert loads_script_known_gaps(text) == [GAP, OTHER_GAP]
    assert json.loads(text.splitlines()[1].rstrip(",")) == {
        "release": "157",
        "case": "page",
        "kind": "shape",
        "field": "Navigator.prototype own share",
        "baseline": ["wec", ["function", 0]],
        "mitto": None,
    }


@pytest.mark.parametrize(
    "text",
    [
        "not json",
        "{}",
        '[{"release": "157"}]',
        '[{"release": "157", "case": "page", "kind": "odd", "field": "f", "baseline": 1, "mitto": 2}]',
        '[{"release": "157", "case": "page", "kind": "shape", "field": "f", "baseline": 1, "mitto": 2, "x": 3}]',
    ],
)
def test_a_bad_list_is_refused(text: str) -> None:
    with pytest.raises(KnownGapsError):
        loads_script_known_gaps(text)


def test_a_run_reports_what_is_not_listed_and_what_is_gone_with_its_kind() -> None:
    found = mismatches([GAP], [OTHER_GAP])

    report = str(found)
    assert found.unlisted == [GAP]
    assert found.gone == [OTHER_GAP]
    assert "Chrome 157, page, shape, Navigator.prototype own share" in report
    assert "Chrome 156, page, behaviour, history get length" in report
    assert "mitto:    (nothing)" in report


def test_a_difference_names_its_kind() -> None:
    assert ScriptDifference("f", GapKind.PERSONA, 1, 2).kind == "persona"
