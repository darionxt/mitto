"""Tests for the forms of the values that the normal form hides, and for the check of recordings against them."""

import json
from pathlib import Path

import pytest

from tests.capture import samples
from tests.capture.test_script_normal_form import record_of
from tools.capture.machine_forms import (
    FORMS,
    SCRIPT_FORMS,
    check_directory,
    check_script_records,
    count_forms,
    hidden_values,
    main,
)
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE
from tools.capture.probe import RecordValue, dumps_record
from tools.capture.recording import Recording, raw_form
from tools.capture.script_normal_form import (
    DATE_STRINGS,
    DATE_TIME_FORMAT,
    DATE_TIME_FORMAT_HOURS,
    HIGH_ENTROPY_VALUES,
    MACHINE_PLACES,
    MEDIA_AGREEMENT,
    MEDIA_INTEGERS,
    MEDIA_KEYWORDS,
    TIME_ZONE_OFFSETS,
    Computed,
    Getter,
    Place,
    dumps_script_raw_record,
    script_normal_form,
)
from tools.capture.script_page import SCRIPT_DIRECTORY

_PAGE_URL = "https://capture.test:8448/script/page"


def write_raw(path: Path, *lines: str) -> None:
    """Write a raw recording of one HTTP/1.1 request with these header lines, as the capture kit writes it."""
    head = "".join(["GET /fetch HTTP/1.1\r\n", *(f"{line}\r\n" for line in lines), "\r\n"]).encode()
    hello = samples.client_hello_records([samples.server_name()])
    recording = Recording(samples.HOST, samples.AUTHORITY, (samples.replay_connection(1, hello, "http/1.1", head),))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(raw_form(recording)), encoding="utf-8")


def test_each_hint_the_normal_form_hides_has_a_form() -> None:
    assert set(FORMS) == HINTS_FROM_THE_MACHINE


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("device-memory", "8"),
        ("sec-ch-device-memory", "32"),
        ("dpr", "1"),
        ("sec-ch-dpr", "1.25"),
        ("dpr", "1.5"),
        ("viewport-width", "1920"),
        ("sec-ch-viewport-height", "945"),
        ("rtt", "0"),
        ("rtt", "50"),
        ("rtt", "150"),
        ("rtt", "2950"),
        ("rtt", "3000"),
        ("downlink", "0"),
        ("downlink", "0.05"),
        ("downlink", "2.5"),
        ("downlink", "2.65"),
        ("downlink", "10"),
        ("ect", "4g"),
        ("ect", "slow-2g"),
        ("sec-ch-prefers-color-scheme", "dark"),
        ("sec-ch-prefers-reduced-motion", "no-preference"),
        ("sec-ch-prefers-reduced-transparency", "reduce"),
        ("sec-ch-ua-platform-version", '"19.0.0"'),
        ("sec-ch-ua-platform-version", '"10.0.0"'),
    ],
)
def test_a_value_as_chrome_writes_it_has_its_form(name: str, value: str) -> None:
    assert FORMS[name].fits(value)


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("device-memory", "0.5"),
        ("device-memory", "64"),
        ("dpr", "1.0"),
        ("dpr", "1.50"),
        ("dpr", ".5"),
        ("viewport-width", "0"),
        ("viewport-width", "1920.0"),
        ("rtt", "25"),
        ("rtt", "050"),
        ("rtt", "3050"),
        ("downlink", "2.50"),
        ("downlink", "2.66"),
        ("downlink", "10.0"),
        ("downlink", "10.5"),
        ("ect", "5g"),
        ("sec-ch-prefers-color-scheme", "no-preference"),
        ("sec-ch-prefers-reduced-motion", "reduced"),
        ("sec-ch-ua-platform-version", "19.0.0"),
        ("sec-ch-ua-platform-version", '"19.0"'),
        ("sec-ch-ua-platform-version", '"19.0.1"'),
        ("sec-ch-ua-platform-version", '"10.0.19045"'),
    ],
)
def test_a_value_written_otherwise_does_not_have_its_form(name: str, value: str) -> None:
    assert not FORMS[name].fits(value)


def test_the_values_that_the_normal_form_hides_come_out_by_lower_case_name(tmp_path: Path) -> None:
    path = tmp_path / "http1.raw.json"
    write_raw(path, "User-Agent: x", "Sec-CH-DPR: 1.5", 'sec-ch-ua-arch: "x86"', 'sec-ch-ua-platform-version: "19.0.0"')

    assert list(hidden_values(json.loads(path.read_text(encoding="utf-8")))) == [
        ("sec-ch-dpr", "1.5"),
        ("sec-ch-ua-platform-version", '"19.0.0"'),
    ]


def test_each_hint_is_counted_with_the_values_that_lack_its_form() -> None:
    counts = {count.name: count for count in count_forms([("rtt", "50"), ("rtt", "55"), ("ect", "4g")])}

    assert (counts["rtt"].values, counts["rtt"].misfits) == (2, 1)
    assert (counts["ect"].values, counts["ect"].misfits) == (1, 0)
    assert (counts["dpr"].values, counts["dpr"].misfits) == (0, 0)
    assert list(counts) == sorted(FORMS)


def test_a_directory_is_read_with_the_recordings_in_its_cases(tmp_path: Path) -> None:
    write_raw(tmp_path / "http1.raw.json", "rtt: 100")
    write_raw(tmp_path / "cases" / "client_hints_http1.raw.json", "rtt: 150", "ect: 4g")
    (tmp_path / "cases" / "client_hints_http1.json").write_text('{"rtt": "55"}', encoding="utf-8")

    recordings, counts = check_directory(tmp_path)

    assert recordings == 2
    assert {count.name: count.values for count in counts if count.values} == {"rtt": 2, "ect": 1}


def test_recordings_whose_values_have_their_forms_pass_and_print_no_value(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    write_raw(tmp_path / "a" / "cases" / "client_hints_http1.raw.json", "viewport-width: 1371", "rtt: 1450")

    assert main([str(tmp_path / "a")]) == 0

    printed = capsys.readouterr().out
    assert "pass, 2 values in 1 raw recordings" in printed
    assert "  pass  viewport-width: a whole number from 1, 1 values" in printed
    assert "  none  dpr:" in printed
    assert "1371" not in printed
    assert "1450" not in printed


def test_a_value_without_its_form_fails_and_is_not_printed(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write_raw(tmp_path / "a" / "http1.raw.json", "rtt: 1450")
    write_raw(tmp_path / "b" / "http1.raw.json", "rtt: 1475")

    assert main([str(tmp_path / "a"), str(tmp_path / "b")]) == 1

    printed = capsys.readouterr().out
    assert f"{tmp_path / 'a'}: pass" in printed
    assert f"{tmp_path / 'b'}: FAIL" in printed
    assert "  FAIL  rtt: a multiple of 50 from 0 to 3000, 1 of 1 values without it" in printed
    assert "1475" not in printed


def test_recordings_with_no_value_to_check_fail(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write_raw(tmp_path / "http1.raw.json", "User-Agent: x")

    assert main([str(tmp_path)]) == 1
    assert f"{tmp_path}: FAIL, 0 values in 1 raw recordings" in capsys.readouterr().out


# The values of a Script Fingerprint


def windows_values() -> dict[Place, RecordValue]:
    """Return a value as Chrome on Windows gives it at each place of the machine, for a machine of no one's."""
    offsets: list[RecordValue] = [-330] * 12
    values: dict[Place, RecordValue] = {
        Getter("globalThis", "innerWidth"): 1371,
        Getter("globalThis", "innerHeight"): 777,
        Getter("globalThis", "outerWidth"): 1387,
        Getter("globalThis", "outerHeight"): 872,
        Getter("globalThis", "screenX"): 10,
        Getter("globalThis", "screenY"): 10,
        Getter("globalThis", "screenLeft"): -8,
        Getter("globalThis", "screenTop"): 0,
        Getter("globalThis", "devicePixelRatio"): 1.25,
        Getter("screen", "availWidth"): 1536,
        Getter("screen", "availHeight"): 816,
        Getter("screen", "width"): 1536,
        Getter("screen", "height"): 864,
        Getter("screen", "colorDepth"): 24,
        Getter("screen", "pixelDepth"): 24,
        Getter("screen", "availLeft"): 0,
        Getter("screen", "availTop"): 0,
        Getter("screen", "isExtended"): False,
        Getter("screen.orientation", "type"): "landscape-primary",
        Getter("screen.orientation", "angle"): 0,
        Getter("visualViewport", "width"): 1371,
        Getter("visualViewport", "height"): 776.7999877929688,
        Getter('viewport.segments["0"]', "width"): 1371,
        Getter('viewport.segments["0"]', "height"): 777,
        Getter('viewport.segments["0"]', "right"): 1371,
        Getter('viewport.segments["0"]', "bottom"): 777,
        Getter("navigator", "hardwareConcurrency"): 16,
        Getter("navigator", "deviceMemory"): 8,
        Getter("navigator", "maxTouchPoints"): 0,
        Getter("navigator", "cpuPerformance"): 3,
        Getter("navigator.connection", "effectiveType"): "4g",
        Getter("navigator.connection", "rtt"): 50,
        Getter("navigator.connection", "downlink"): 9.45,
        Getter("console.memory", "jsHeapSizeLimit"): 3_760_000_000,
        Getter("performance.memory", "jsHeapSizeLimit"): 4_294_705_152,
        Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)): "19.0.0",
        Computed(MEDIA_INTEGERS, ("color",)): [8],
        Computed(TIME_ZONE_OFFSETS): offsets,
    }
    keywords: dict[str, RecordValue] = {
        "display-state": [],
        "orientation": ["landscape"],
        "pointer": ["fine"],
        "any-pointer": ["fine"],
        "hover": ["hover"],
        "any-hover": ["hover"],
        "color-gamut": ["srgb"],
        "inverted-colors": [],
        "prefers-color-scheme": ["light"],
        "prefers-contrast": ["no-preference"],
        "prefers-reduced-motion": ["no-preference"],
        "dynamic-range": ["standard"],
        "video-dynamic-range": [],
        "prefers-reduced-transparency": ["no-preference"],
        "forced-colors": ["none"],
    }
    values.update({Computed(MEDIA_KEYWORDS, (name,)): value for name, value in keywords.items()})
    for key in (DATE_TIME_FORMAT, DATE_TIME_FORMAT_HOURS):
        values[Computed(key, ("timeZone",))] = "Asia/Calcutta"
    agreements = (
        "width = innerWidth",
        "height = innerHeight",
        "aspect-ratio = innerWidth / innerHeight",
        "device-width = screen.width",
        "device-height = screen.height",
        "device-aspect-ratio = screen.width / screen.height",
    )
    values.update({Computed(MEDIA_AGREEMENT, (name,)): name != "height = innerHeight" for name in agreements})
    for day, weekday, month, date in (("winter", "Thu", "Jan", "1/15/2026"), ("summer", "Wed", "Jul", "7/15/2026")):
        strings = {
            "toString": f"{weekday} {month} 15 2026 17:30:00 GMT+0530 (India Standard Time)",
            "toTimeString": "17:30:00 GMT+0530 (India Standard Time)",
            "toLocaleString": f"{date}, 5:30:00\u202fPM",
            "toLocaleDateString": date,
            "toLocaleTimeString": "5:30:00\u202fPM",
            "timeZoneName long": f"{date}, India Standard Time",
        }
        values.update({Computed(DATE_STRINGS, (day, name)): value for name, value in strings.items()})
    return values


def test_each_value_the_normal_form_of_a_script_fingerprint_hides_has_a_form() -> None:
    assert list(SCRIPT_FORMS) == list(MACHINE_PLACES)


def test_a_value_as_chrome_on_windows_gives_it_a_page_has_its_form() -> None:
    values = windows_values()

    assert set(values) == set(SCRIPT_FORMS)
    assert [str(place) for place, value in values.items() if not SCRIPT_FORMS[place].fits(value)] == []


@pytest.mark.parametrize(
    ("place", "value"),
    [
        (Getter("globalThis", "innerWidth"), 0),
        (Getter("globalThis", "innerWidth"), 1371.5),
        (Getter("globalThis", "innerWidth"), True),
        (Getter("globalThis", "devicePixelRatio"), 0.5),
        (Getter("globalThis", "devicePixelRatio"), "1.25"),
        (Getter("screen", "colorDepth"), 32),
        (Getter("screen.orientation", "angle"), 45),
        (Getter("visualViewport", "width"), 0),
        (Getter("navigator", "hardwareConcurrency"), 65),
        (Getter("navigator", "deviceMemory"), 0.5),
        (Getter("navigator", "cpuPerformance"), 5),
        (Getter("navigator.connection", "rtt"), 75),
        (Getter("navigator.connection", "downlink"), 10.5),
        (Getter("console.memory", "jsHeapSizeLimit"), 1024),
        (Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)), "10.0.19045"),
        (Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)), '"19.0.0"'),
        (Computed(MEDIA_KEYWORDS, ("prefers-color-scheme",)), ["light", "dark"]),
        (Computed(MEDIA_KEYWORDS, ("any-pointer",)), ["fine", "coarse"]),
        (Computed(MEDIA_INTEGERS, ("color",)), [8, 10]),
        (Computed(DATE_TIME_FORMAT, ("timeZone",)), "America/Detroit"),
        (Computed(DATE_TIME_FORMAT, ("timeZone",)), "Mars/Olympus_Mons"),
        (Computed(TIME_ZONE_OFFSETS), [300] * 11),
        (Computed(TIME_ZONE_OFFSETS), [301] * 12),
        (Computed(DATE_STRINGS, ("winter", "toLocaleDateString")), "15/01/2026"),
        (Computed(DATE_STRINGS, ("summer", "toString")), "Wed Jul 15 2026 08:00:00 GMT-0400"),
    ],
    ids=str,
)
def test_a_value_written_otherwise_does_not_have_the_form_of_its_place(place: Place, value: RecordValue) -> None:
    assert not SCRIPT_FORMS[place].fits(value)


def write_script(
    directory: Path, values: dict[Place, RecordValue], *, form: dict[str, RecordValue] | None = None
) -> None:
    """Write a raw record of a Script Fingerprint with these values, and its Normal Form beside it unless form says
    another, as a recording session does."""
    record = record_of(values)
    path = directory / SCRIPT_DIRECTORY / "page.raw.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dumps_script_raw_record(_PAGE_URL, ["127.0.0.1"], record), encoding="utf-8")
    normal = script_normal_form(record, _PAGE_URL) if form is None else form
    path.with_name("page.json").write_text(dumps_record(normal), encoding="utf-8")


def test_the_raw_records_of_a_script_fingerprint_are_no_raw_recordings_of_the_wire(tmp_path: Path) -> None:
    write_raw(tmp_path / "http1.raw.json", "rtt: 100")
    write_script(tmp_path, windows_values())

    recordings, counts = check_directory(tmp_path)
    (check,) = check_script_records(tmp_path)

    assert (recordings, sum(count.values for count in counts)) == (1, 1)
    assert (check.raw_name, check.form_name, check.values) == ("script/page.raw.json", "script/page.json", 73)
    assert check.passed


def test_a_script_fingerprint_whose_values_have_their_forms_passes_and_prints_no_value(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    write_raw(tmp_path / "a" / "http1.raw.json", "rtt: 100")
    write_script(tmp_path / "a", windows_values())

    assert main([str(tmp_path / "a")]) == 0

    printed = capsys.readouterr().out
    assert "pass, 1 values in 1 raw recordings, and 1 raw records of a Script Fingerprint" in printed
    assert "  script/page.raw.json: pass, 73 values" in printed
    assert "    pass  screen.width: a whole number of device-independent pixels from 1, 1 values" in printed
    assert "    pass  none of 24 values of the machine shows in script/page.json" in printed
    for value in ("1536", "1371", "Asia/Calcutta", "India Standard Time", "330", "4294705152"):
        assert value not in printed


def test_a_script_fingerprint_alone_is_enough_to_check(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write_script(tmp_path, windows_values())

    assert main([str(tmp_path)]) == 0

    printed = capsys.readouterr().out
    assert "pass, 0 values in 0 raw recordings, and 1 raw records of a Script Fingerprint" in printed
    assert "  none  rtt:" not in printed


def test_a_value_of_a_script_fingerprint_without_its_form_fails_and_is_not_printed(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    write_script(tmp_path, {**windows_values(), Getter("screen", "colorDepth"): 31})

    assert main([str(tmp_path)]) == 1

    printed = capsys.readouterr().out
    assert "  script/page.raw.json: FAIL, 73 values" in printed
    assert "    FAIL  screen.colorDepth: 24, or 30 on a display with HDR, 1 of 1 values without it" in printed
    assert "31" not in printed


def test_a_value_of_the_machine_that_shows_in_the_normal_form_fails(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    values = windows_values()
    form = record_of({**dict.fromkeys(values, "<machine>"), Getter("document", "width"): 1371})
    write_script(tmp_path, values, form=form)

    assert main([str(tmp_path)]) == 1

    printed = capsys.readouterr().out
    assert "    FAIL  a value of the machine shows in script/page.json, at document get width" in printed
    assert "1371" not in printed


def test_a_raw_record_with_no_normal_form_beside_it_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write_script(tmp_path, windows_values())
    (tmp_path / SCRIPT_DIRECTORY / "page.json").unlink()

    assert main([str(tmp_path)]) == 1
    assert "    FAIL  no script/page.json to look in" in capsys.readouterr().out
