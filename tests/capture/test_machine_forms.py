"""Tests for the forms of the values that the normal form hides, and for the check of recordings against them."""

import json
from pathlib import Path

import pytest

from tests.capture import samples
from tools.capture.machine_forms import FORMS, check_directory, count_forms, hidden_values, main
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE
from tools.capture.recording import Recording, raw_form


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
