"""Tests of what the Baseline of a later Recording Case of a Script Fingerprint keeps: what differs from the first's."""

import copy
import functools
import json
from collections.abc import Callable

import pytest

from tools.capture.probe import RecordValue, read_record
from tools.capture.script_delta import (
    differs_from,
    dumps_script_delta,
    read_script_delta,
    script_delta,
    with_script_delta,
)
from tools.parity import BASELINES_DIRECTORY

type _Record = dict[str, RecordValue]


@functools.cache
def _page_text() -> str:
    return (BASELINES_DIRECTORY / "157" / "script" / "page.json").read_text(encoding="utf-8")


@pytest.fixture
def page() -> _Record:
    """Return the Normal Form of Chrome 157's top-level page, the first Recording Case."""
    return read_record(_page_text())


def entry(record: _Record, path: str) -> dict[str, RecordValue]:
    objects = record["objects"]
    assert isinstance(objects, list)
    for item in objects:
        assert isinstance(item, dict)
        if item["path"] == path:
            return item
    raise AssertionError(f"no {path}")


def items(record: _Record, path: str, section: str) -> list[list[RecordValue]]:
    found = entry(record, path)[section]
    assert isinstance(found, list)
    return [item for item in found if isinstance(item, list)]


def canonical(record: _Record) -> str:
    """Write a record with its objects by path, since a delta does not keep the order of objects."""
    objects = record["objects"]
    assert isinstance(objects, list)
    by_path = sorted(objects, key=lambda item: str(item["path"]) if isinstance(item, dict) else "")
    return json.dumps({**{key: record[key] for key in sorted(record)}, "objects": by_path})


def back(page: _Record, form: _Record) -> _Record:
    """Write what form differs in as its file is written, read it, and make the form again from the page's."""
    return with_script_delta(page, read_script_delta(dumps_script_delta(script_delta(page, form, "page"))))


def changed(page: _Record, change: Callable[[_Record], None]) -> _Record:
    form = copy.deepcopy(page)
    change(form)
    return form


def set_getter(path: str, name: str, value: RecordValue) -> Callable[[_Record], None]:
    def change(form: _Record) -> None:
        for item in items(form, path, "get"):
            if item[0] == name:
                item[1] = value

    return change


def test_a_case_like_the_first_keeps_nothing(page: _Record) -> None:
    delta = script_delta(page, copy.deepcopy(page), "page")

    assert delta == {"format": 1, "differs from": "page", "objects": [], "gone": [], "values": {}}
    assert canonical(back(page, page)) == canonical(page)


def test_a_getter_that_gives_another_value_is_all_that_its_object_keeps(page: _Record) -> None:
    form = changed(page, set_getter("globalThis", "innerWidth", 300))

    delta = script_delta(page, form, "page")

    assert delta["objects"] == [{"path": "globalThis", "get": [["innerWidth", 300]]}]
    assert canonical(back(page, form)) == canonical(form)


def test_true_is_not_1(page: _Record) -> None:
    form = changed(page, set_getter("history", "length", True))

    assert canonical(back(page, form)) == canonical(form)
    assert items(back(page, form), "history", "get")[0][1] is True


def test_an_object_that_only_the_case_has_is_kept_whole(page: _Record) -> None:
    top: dict[str, RecordValue] = {"path": "top", "kind": "instance", "tag": "Object", "proto": None, "own": []}

    def change(form: _Record) -> None:
        set_getter("globalThis", "top", ["object", "top"])(form)
        _append(form, top)

    form = changed(page, change)

    delta = script_delta(page, form, "page")

    objects = delta["objects"]
    assert isinstance(objects, list)
    assert objects[-1] == top
    assert canonical(back(page, form)) == canonical(form)


def _append(form: _Record, item: dict[str, RecordValue]) -> None:
    objects = form["objects"]
    assert isinstance(objects, list)
    objects.append(item)


def test_an_object_that_only_the_first_has_is_named_as_gone(page: _Record) -> None:
    def drop(form: _Record) -> None:
        objects = form["objects"]
        assert isinstance(objects, list)
        form["objects"] = [item for item in objects if isinstance(item, dict) and item["path"] != "viewport.segments"]

    form = changed(page, drop)

    assert script_delta(page, form, "page")["gone"] == ["viewport.segments"]
    assert canonical(back(page, form)) == canonical(form)


def test_items_and_keys_that_are_gone_new_or_moved_come_back_in_the_cases_order(page: _Record) -> None:
    def change(form: _Record) -> None:
        navigator = entry(form, "Navigator.prototype")
        own = items(form, "Navigator.prototype", "own")
        navigator["own"] = [own[3], *own[5:], own[0], ["madeUp", "wec", 1]]
        navigator.pop("calls", None)
        navigator["new"] = ["returns", "object"]

    form = changed(page, change)

    delta = script_delta(page, form, "page")

    objects = delta["objects"]
    assert isinstance(objects, list)
    (navigator,) = objects
    assert isinstance(navigator, dict)
    assert navigator["own"] == [["madeUp", "wec", 1]]
    assert "own <order>" in navigator
    assert navigator["new"] == ["returns", "object"]
    gone = navigator["gone"]
    assert isinstance(gone, list)
    assert "calls" in gone
    assert len([name for name in gone if isinstance(name, str) and name.startswith("own ")]) == 3
    assert canonical(back(page, form)) == canonical(form)


def test_values_that_differ_are_kept_whole_and_those_that_are_gone_as_null(page: _Record) -> None:
    def change(form: _Record) -> None:
        values = form["values"]
        assert isinstance(values, dict)
        del values["document.featurePolicy.features()"]
        values["Number: toLocaleString() of 1234567.891"] = {"value": "1.234.567,891"}
        values["a new value"] = {"throws": "Error"}
        form["reports"] = [["deprecation", "Something"]]

    form = changed(page, change)

    delta = script_delta(page, form, "page")

    assert delta["values"] == {
        "Number: toLocaleString() of 1234567.891": {"value": "1.234.567,891"},
        "a new value": {"throws": "Error"},
        "document.featurePolicy.features()": None,
    }
    assert delta["reports"] == [["deprecation", "Something"]]
    assert canonical(back(page, form)) == canonical(form)


def test_each_item_is_a_line_of_its_own(page: _Record) -> None:
    form = changed(page, set_getter("globalThis", "innerWidth", 300))

    text = dumps_script_delta(script_delta(page, form, "page"))

    assert text.splitlines() == [
        "{",
        '"format": 1,',
        '"differs from": "page",',
        '"objects": [',
        '{"path": "globalThis",',
        ' "get": [',
        '  ["innerWidth", 300]',
        " ]",
        "}",
        "],",
        '"gone": [',
        "],",
        '"values": {}',
        "}",
    ]


def test_it_names_the_case_it_differs_from(page: _Record) -> None:
    assert differs_from(read_script_delta(dumps_script_delta(script_delta(page, page, "page")))) == "page"


def test_a_delta_of_another_first_case_is_refused(page: _Record) -> None:
    other = changed(page, lambda form: form.update(objects=[]))
    delta = script_delta(page, other, "page")

    with pytest.raises(ValueError, match="the first case has no object at"):
        with_script_delta(other, delta)


@pytest.mark.parametrize(
    "text",
    [
        "[]",
        '{"format": 1, "objects": [], "values": {}, "reports": []}',
        '{"format": 1, "differs from": "page", "objects": [], "values": {}}',
    ],
)
def test_what_is_no_delta_is_refused(text: str) -> None:
    with pytest.raises(ValueError, match="not a delta"):
        read_script_delta(text)


def test_a_list_with_a_key_twice_is_refused(page: _Record) -> None:
    def twice(form: _Record) -> None:
        own = items(form, "Navigator.prototype", "own")
        entry(form, "Navigator.prototype")["own"] = [*own, own[0]]

    with pytest.raises(ValueError, match="twice"):
        script_delta(page, changed(page, twice), "page")
