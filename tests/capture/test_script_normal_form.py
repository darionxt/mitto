"""Tests of the Normal Form of a Script Fingerprint, of the raw record a recording session keeps, and of the check that
none of the recording machine's values shows in a Normal Form."""

import json
from collections import defaultdict
from pathlib import Path

import pytest

from tools.capture.probe import RecordValue, dumps_record, read_record
from tools.capture.script_normal_form import (
    DATE_STRINGS,
    DATE_TIME_FORMAT,
    MACHINE_PLACES,
    PORT_PLACEHOLDER,
    RUN_PLACEHOLDER,
    RUN_PLACES,
    TIME_ZONE_OFFSETS,
    Computed,
    Getter,
    Place,
    dumps_script_raw_record,
    machine_values,
    machine_values_shown,
    read_script_raw_record,
    script_normal_form,
)
from tools.capture.script_page import SCRIPT_DIRECTORY
from tools.parity import BASELINES_DIRECTORY, load_baselines

_PAGE_URL = "https://capture.test:8448/script/page"
_BASELINE_BUDGET_BYTES = 1_500_000


def record_of(values: dict[Place, RecordValue], *, kept: bool = True) -> dict[str, RecordValue]:
    """Return a record that holds each value at its place, with a value that no Normal Form hides beside each, unless
    kept is false."""
    getters: defaultdict[str, list[RecordValue]] = defaultdict(list)
    computed: dict[str, RecordValue] = {}
    for place, value in values.items():
        if isinstance(place, Getter):
            assert place.name is not None
            getters[place.path].append([place.name, value])
            continue
        if not place.keys:
            computed[place.key] = {"value": value}
            continue
        entry = computed.setdefault(place.key, {"value": {}})
        assert isinstance(entry, dict)
        inner = entry["value"]
        for key in place.keys[:-1]:
            assert isinstance(inner, dict)
            inner = inner.setdefault(key, {})
        assert isinstance(inner, dict)
        inner[place.keys[-1]] = value
    objects: list[RecordValue] = []
    for path, items in getters.items():
        if kept:
            items.append(["kept", "a value of Chrome's"])
        objects.append({"path": path, "kind": "instance", "get": items})
    if kept:
        computed["Number: toLocaleString() of 1234567.891"] = {"value": "1,234,567.891"}
    return {"format": 1, "objects": objects, "values": computed, "reports": []}


def machine_record(seed: int) -> dict[str, RecordValue]:
    """Return a record with a value of its own at each place of the machine, which another seed changes."""
    return record_of({place: f"value {seed} of {place}" for place in MACHINE_PLACES})


def getters_of(record: dict[str, RecordValue], path: str) -> dict[str, RecordValue]:
    objects = record["objects"]
    assert isinstance(objects, list)
    for entry in objects:
        assert isinstance(entry, dict)
        if entry["path"] == path:
            items = entry["get"]
            assert isinstance(items, list)
            return {str(item[0]): item[1] for item in items if isinstance(item, list)}
    raise AssertionError(f"no {path}")


# What the Normal Form hides


def test_each_value_of_the_machine_becomes_machine_in_its_place() -> None:
    form = script_normal_form(machine_record(1), _PAGE_URL)

    assert [value for _, value in machine_values(form)] == ["<machine>"] * len(MACHINE_PLACES)
    assert getters_of(form, "navigator")["kept"] == "a value of Chrome's"
    values = form["values"]
    assert isinstance(values, dict)
    assert values["Number: toLocaleString() of 1234567.891"] == {"value": "1,234,567.891"}


def test_two_machines_have_the_same_normal_form() -> None:
    assert script_normal_form(machine_record(1), _PAGE_URL) == script_normal_form(machine_record(2), _PAGE_URL)


def test_each_value_that_changes_from_run_to_run_becomes_run() -> None:
    record = record_of(
        {
            Getter("performance", "timeOrigin"): 1794491014910.5,
            Getter("performance.memory", "usedJSHeapSize"): 10_000_000,
            Getter("document", "lastModified"): "11/12/2026 21:43:35",
            Getter("navigation.currentEntry", "key"): "e7df647a-d6e8-40df-ad21-900f5bd43441",
            Getter("performance.timing", "navigationStart"): 1794491014910,
            Getter("performance.timing", "loadEventEnd"): 1794491015000,
        }
    )

    form = script_normal_form(record, _PAGE_URL)

    assert getters_of(form, "performance") == {"timeOrigin": RUN_PLACEHOLDER, "kept": "a value of Chrome's"}
    assert getters_of(form, "performance.memory")["usedJSHeapSize"] == RUN_PLACEHOLDER
    assert getters_of(form, "document")["lastModified"] == RUN_PLACEHOLDER
    assert getters_of(form, "navigation.currentEntry")["key"] == RUN_PLACEHOLDER
    # Each getter of performance.timing, this test's own value too.
    assert set(getters_of(form, "performance.timing").values()) == {RUN_PLACEHOLDER}


def test_the_heap_limit_of_performance_memory_is_the_machines_and_its_figures_are_the_runs() -> None:
    record = record_of(
        {
            Getter("performance.memory", "jsHeapSizeLimit"): 4_294_705_152,
            Getter("performance.memory", "totalJSHeapSize"): 12_345_678,
        }
    )

    assert getters_of(script_normal_form(record, _PAGE_URL), "performance.memory") == {
        "jsHeapSizeLimit": "<machine>",
        "totalJSHeapSize": RUN_PLACEHOLDER,
        "kept": "a value of Chrome's",
    }


def test_a_getter_that_threw_at_a_place_of_the_machine_or_of_a_run_stays_as_it_is() -> None:
    failure: RecordValue = ["throws", "TypeError: Not implemented."]
    record = record_of(
        {
            Getter("navigator", "hardwareConcurrency"): failure,
            Getter("navigator", "deviceMemory"): 8,
            Getter("performance", "timeOrigin"): failure,
        }
    )

    form = script_normal_form(record, _PAGE_URL)

    assert getters_of(form, "navigator")["hardwareConcurrency"] == failure
    assert getters_of(form, "navigator")["deviceMemory"] == "<machine>"
    assert getters_of(form, "performance")["timeOrigin"] == failure
    assert script_normal_form(record_of({Getter("navigator", "hardwareConcurrency"): 8}), _PAGE_URL) != form


def test_a_place_that_a_record_lacks_or_that_threw_is_left_as_it_is() -> None:
    record = record_of({})
    values = record["values"]
    assert isinstance(values, dict)
    values[DATE_TIME_FORMAT] = {"throws": "TypeError: Not implemented."}
    values[DATE_STRINGS] = {"value": "not an object"}
    values[TIME_ZONE_OFFSETS] = {"timeout": True}

    assert script_normal_form(record, _PAGE_URL) == record


def test_the_record_itself_stays_as_it_was() -> None:
    record = machine_record(1)
    text = json.dumps(record)

    script_normal_form(record, _PAGE_URL)

    assert json.dumps(record) == text


def test_the_places_of_the_machine_and_of_the_run_are_each_named_once() -> None:
    places = [*MACHINE_PLACES, *RUN_PLACES]

    assert len(set(places)) == len(places)
    assert all(place.name is not None for place in MACHINE_PLACES if isinstance(place, Getter))


def test_machine_values_come_in_the_order_of_their_places() -> None:
    record = record_of(
        {
            Computed(DATE_TIME_FORMAT, ("timeZone",)): "Europe/Berlin",
            Getter("globalThis", "innerWidth"): 1280,
            Getter("screen", "width"): 1536,
        }
    )

    assert list(machine_values(record)) == [
        (Getter("globalThis", "innerWidth"), 1280),
        (Getter("screen", "width"), 1536),
        (Computed(DATE_TIME_FORMAT, ("timeZone",)), "Europe/Berlin"),
    ]


def test_a_place_is_written_as_where_it_is_in_the_record() -> None:
    assert str(Getter("screen.orientation", "type")) == "screen.orientation.type"
    assert str(Getter("performance.timing", None)) == "performance.timing"
    assert str(Computed(DATE_STRINGS, ("winter", "toString"))) == f'{DATE_STRINGS}["winter"]["toString"]'


# The page's origin


def location_record(port: str) -> dict[str, RecordValue]:
    origin = "https://capture.test" + (f":{port}" if port else "")
    return record_of(
        {
            Getter("location", "href"): f"{origin}/script/page",
            Getter("location", "origin"): origin,
            Getter("location", "host"): origin.removeprefix("https://"),
            Getter("location", "hostname"): "capture.test",
            Getter("location", "port"): port,
            Getter("location", "pathname"): "/script/page",
            Getter("document", "domain"): "capture.test",
        }
    )


def test_the_pages_origin_name_and_port_go_behind_placeholders() -> None:
    form = script_normal_form(location_record("8448"), _PAGE_URL)

    assert getters_of(form, "location") == {
        "href": "<origin>/script/page",
        "origin": "<origin>",
        "host": "<authority>",
        "hostname": "<host>",
        "port": PORT_PLACEHOLDER,
        "pathname": "/script/page",
        "kept": "a value of Chrome's",
    }
    assert getters_of(form, "document")["domain"] == "<host>"


def test_a_longer_name_or_port_is_not_the_pages() -> None:
    record = record_of(
        {
            Getter("a", "same_site"): "https://same-site.capture.test:8448/x",
            Getter("a", "other_port"): "https://capture.test:84480/x",
            Getter("a", "longer_name"): "capture.tests",
            Getter("a", "in_a_sentence"): "Failed to reach capture.test:8448.",
        }
    )

    assert getters_of(script_normal_form(record, _PAGE_URL), "a") == {
        "same_site": "https://same-site.capture.test:8448/x",
        "other_port": "https://<host>:84480/x",
        "longer_name": "capture.tests",
        "in_a_sentence": "Failed to reach <authority>.",
        "kept": "a value of Chrome's",
    }


def test_the_normal_form_does_not_depend_on_the_pages_port() -> None:
    first = script_normal_form(location_record("8448"), "https://capture.test:8448/script/page")
    second = script_normal_form(location_record("50123"), "https://capture.test:50123/script/page")

    assert getters_of(first, "location") == getters_of(second, "location")


def test_a_page_at_its_schemes_own_port_has_no_port_to_hide() -> None:
    form = script_normal_form(location_record(""), "https://capture.test/script/page")

    assert getters_of(form, "location")["host"] == "<authority>"
    assert getters_of(form, "location")["port"] == ""


@pytest.mark.parametrize("url", ["https://capture.test:443/script/page", "http://capture.test:80/script/page"])
def test_a_port_that_is_the_schemes_own_is_left_out_as_a_page_leaves_it_out(url: str) -> None:
    scheme = url.partition(":")[0]
    record = record_of(
        {
            Getter("location", "origin"): f"{scheme}://capture.test",
            Getter("location", "href"): f"{scheme}://capture.test/script/page",
            Getter("location", "port"): "",
        }
    )

    assert getters_of(script_normal_form(record, url), "location") == {
        "origin": "<origin>",
        "href": "<origin>/script/page",
        "port": "",
        "kept": "a value of Chrome's",
    }


def test_a_page_with_no_origin_keeps_its_strings() -> None:
    record = location_record("8448")

    assert getters_of(script_normal_form(record, None), "location") == getters_of(record, "location")


def test_an_address_with_no_origin_is_refused() -> None:
    with pytest.raises(ValueError, match="has no origin"):
        script_normal_form(location_record("8448"), "about:blank")


# The values of the machine that a Normal Form shows


def leaky_record() -> dict[str, RecordValue]:
    return record_of(
        {
            Getter("globalThis", "innerWidth"): 1371,
            Getter("globalThis", "devicePixelRatio"): 1.25,
            Getter("navigator", "hardwareConcurrency"): 8,
            Computed(DATE_TIME_FORMAT, ("timeZone",)): "Europe/Berlin",
            Computed(TIME_ZONE_OFFSETS): [-60, -60, -60, -120, -120, -120, -120, -120, -120, -120, -60, -60],
            Computed("matchMedia(): the keywords that match", ("prefers-color-scheme",)): ["dark"],
        }
    )


def test_a_normal_form_shows_none_of_the_values_it_hides() -> None:
    raw = leaky_record()

    sought, shown = machine_values_shown(raw, script_normal_form(raw, _PAGE_URL))

    # The width, the zone, its offsets and the one of them of 100 or more. A small number, a scale and a keyword could be
    # anything's.
    assert (sought, shown) == (4, [])


def test_a_value_of_the_machine_that_another_place_holds_shows() -> None:
    raw = leaky_record()
    form = script_normal_form(raw, _PAGE_URL)
    objects = form["objects"]
    assert isinstance(objects, list)
    objects.append({"path": "document.body", "kind": "instance", "own": [["width", "w-c", 1371]], "get": [["x", 1371]]})
    objects.append({"path": "Intl", "kind": "instance", "calls": [["f", ["throws", "RangeError: Europe/Berlin?"]]]})
    values = form["values"]
    assert isinstance(values, dict)
    values["offsets"] = {"value": [-60, -60, -60, -120, -120, -120, -120, -120, -120, -120, -60, -60]}

    _, shown = machine_values_shown(raw, form)

    # Not the number among the own properties, where an interface's constants are.
    assert shown == ["document.body get x", "Intl calls f", 'values["offsets"]["value"]']


def test_what_shows_is_named_by_its_place_and_never_by_its_value() -> None:
    raw = leaky_record()
    form = record_of({Getter("screen", "availWidth"): 1371})

    _, shown = machine_values_shown(raw, form)

    assert shown == ["screen get availWidth"]
    assert "1371" not in " ".join(shown)


# The raw record


def test_a_raw_record_reads_back_as_it_was_written() -> None:
    record = machine_record(1)

    text = dumps_script_raw_record(_PAGE_URL, ["127.0.0.1"], record)
    raw = read_script_raw_record(text)

    assert (raw.url, raw.peers, raw.record) == (_PAGE_URL, ("127.0.0.1",), record)
    # The record in it is written as a Baseline is, a line for each property.
    assert text.count("\n") > dumps_record(record).count("\n")


@pytest.mark.parametrize("text", ["[]", '{"url": "x", "peers": []}', '{"url": 1, "peers": [], "record": {}}'])
def test_what_is_no_raw_record_is_refused(text: str) -> None:
    with pytest.raises(ValueError, match="not a raw record"):
        read_script_raw_record(text)


# The Baselines


def test_the_wires_parity_checks_do_not_read_a_baseline_of_a_script_fingerprint(tmp_path: Path) -> None:
    path = tmp_path / "157" / SCRIPT_DIRECTORY / "page.json"
    path.parent.mkdir(parents=True)
    path.write_text(dumps_record(script_normal_form(machine_record(1), _PAGE_URL)), encoding="utf-8")

    assert load_baselines(tmp_path) == []


_SCRIPT_BASELINES = sorted(BASELINES_DIRECTORY.glob(f"*/{SCRIPT_DIRECTORY}/*.json"))


@pytest.mark.parametrize("path", _SCRIPT_BASELINES, ids=lambda path: path.relative_to(BASELINES_DIRECTORY).as_posix())
def test_each_baseline_of_a_script_fingerprint_is_a_normal_form_within_its_budget(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    form = read_record(text)

    assert len(text.encode()) <= _BASELINE_BUDGET_BYTES
    assert dumps_record(form) == text
    assert all(value == "<machine>" for _, value in machine_values(form))
    assert "capture.test" not in text
