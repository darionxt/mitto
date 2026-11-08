"""Tests of the script that writes the table a Session's Machine is drawn from. They need no native library or network."""

import dataclasses
import json
from fractions import Fraction
from pathlib import Path
from typing import Any

import pytest

from tools.release.machines import (
    STATISTICS,
    TABLE_PATH,
    MachineTableError,
    Source,
    approximated_device_memory,
    display_shares,
    dumps_table,
    table_data,
    write_table,
)


def rows(data: dict[str, Any], group: str) -> list[dict[str, Any]]:
    return data["groups"][group]["rows"]


def test_the_table_is_byte_for_byte_what_the_statistics_write() -> None:
    assert dumps_table(table_data()).encode() == TABLE_PATH.read_bytes()


def test_writing_the_table_writes_the_file(tmp_path: Path) -> None:
    path = write_table(path=tmp_path / "table.json")

    assert path.read_bytes() == TABLE_PATH.read_bytes()


def test_the_table_is_json_with_one_line_for_each_row() -> None:
    text = dumps_table(table_data())
    data = table_data()

    assert json.loads(text) == data
    assert text.count('"weight"') == sum(len(group["rows"]) for group in data["groups"].values())
    assert text.count("\n") == len(text.splitlines())


# As third_party/blink/common/device_memory/approximated_device_memory_unittest.cc has them, but for Android.
@pytest.mark.parametrize(
    ("mib", "gib"),
    [
        (128, 2),
        (256, 2),
        (510, 2),
        (512, 2),
        (640, 2),
        (768, 2),
        (1000, 2),
        (1024, 2),
        (1536, 2),
        (2000, 2),
        (2048, 2),
        (3000, 2),
        (5120, 4),
        (8192, 8),
        (16384, 16),
        (32768, 32),
        (64385, 32),
    ],
)
def test_the_memory_is_rounded_as_chrome_on_a_desktop_rounds_it(mib: int, gib: int) -> None:
    assert approximated_device_memory(mib) == gib


@pytest.mark.parametrize(
    ("gb", "gib"),
    [
        (2, 2),
        (3, 2),
        (4, 4),
        (6, 4),
        (7, 8),
        (8, 8),
        (12, 8),
        (14, 16),
        (15, 16),
        (16, 16),
        (24, 16),
        (31, 32),
        (64, 32),
    ],
)
def test_each_size_of_memory_of_the_firefox_report_counts_as_what_chrome_sends(gb: int, gib: int) -> None:
    memory = Source("", "", {f"{gb}GB": 1.0, "Other": 5.0})
    data = table_data(dataclasses.replace(STATISTICS, memory=memory))

    assert rows(data, "memory") == [{"memory": gib, "weight": 1_000_000}]


def test_the_memory_adds_up_the_sizes_that_chrome_sends_the_same() -> None:
    memory = {row["memory"]: row["weight"] for row in rows(table_data(), "memory")}

    assert list(memory) == [8, 16, 4, 32, 2]
    assert memory[8] == pytest.approx((29.453 + 3.171 + 1.916) / 95.616 * 1_000_000, abs=1)


def test_a_memory_that_is_no_size_is_an_error() -> None:
    with pytest.raises(MachineTableError, match="'lots' is no size of memory"):
        table_data(dataclasses.replace(STATISTICS, memory=Source("", "", {"lots": 1.0})))


def test_windows_11_and_10_claim_the_version_of_their_last_versions_by_statcounters_shares() -> None:
    assert rows(table_data(), "platform_version") == [
        {"platform_version": "19.0.0", "windows": 11, "weight": round(74.2 / 99.42 * 1_000_000)},
        {"platform_version": "10.0.0", "windows": 10, "weight": round(25.22 / 99.42 * 1_000_000)},
    ]


def test_shares_that_do_not_name_windows_10_and_11_are_an_error() -> None:
    with pytest.raises(MachineTableError, match="Win11 and Win10"):
        table_data(dataclasses.replace(STATISTICS, windows_versions=Source("", "", {"Win11": 100.0})))


def test_a_display_is_split_between_scales_by_the_sizes_they_give_in_css_pixels() -> None:
    shares = display_shares({"1920x1080": 30.0, "1536x864": 10.0, "800x600": 9.0}, {"1920x1080": 50.0, "Other": 9.0})

    assert {(display.width, display.height, display.scale): share for display, share in shares.items()} == {
        (1920, 1080, Fraction(1)): pytest.approx(37.5),
        (1920, 1080, Fraction(5, 4)): pytest.approx(12.5),
    }


def test_a_size_in_css_pixels_that_two_displays_give_is_shared_by_their_shares() -> None:
    # 1280x720 is 1920x1080 at 150% and 2560x1440 at 200%.
    shares = display_shares({"1280x720": 4.0, "2560x1440": 6.0}, {"1920x1080": 30.0, "2560x1440": 10.0})

    assert {(display.width, display.scale): share for display, share in shares.items()} == {
        (1920, Fraction(3, 2)): pytest.approx(30.0),
        (2560, Fraction(1)): pytest.approx(10.0 * 6 / 7),
        (2560, Fraction(2)): pytest.approx(10.0 * 1 / 7),
    }


def test_a_display_whose_sizes_in_css_pixels_none_names_is_at_100_percent() -> None:
    shares = display_shares({"1920x1080": 30.0}, {"1024x768": 1.0})

    assert {(display.width, display.scale): share for display, share in shares.items()} == {(1024, Fraction(1)): 1.0}


def test_each_windows_has_the_same_displays_and_windows_at_the_same_weights() -> None:
    displays = rows(table_data(), "display")
    by_windows = {
        windows: [
            (row["window"], row["display_width"], row["display_height"], row["scale"], row["weight"])
            for row in displays
            if row["windows"] == windows
        ]
        for windows in (10, 11)
    }

    assert by_windows[10] == by_windows[11]
    assert sum(weight for *_, weight in by_windows[11]) == pytest.approx(1_000_000, abs=len(by_windows[11]))
    without_weights = [{name: value for name, value in row.items() if name != "weight"} for row in displays]
    assert {
        "windows": 11,
        "window": "maximised",
        "display_width": 1920,
        "display_height": 1080,
        "scale": 1.25,
        "viewport_width": 1536,
        "viewport_height": 864 - 48 - 86,
    } in without_weights
    assert {
        "windows": 10,
        "window": "first",
        "display_width": 1920,
        "display_height": 1080,
        "scale": 1.25,
        "viewport_width": 1050 - 14,
        "viewport_height": 864 - 40 - 20 - 95,
    } in without_weights


def test_only_the_scales_at_which_chromes_bars_were_read_are_drawn() -> None:
    assert {row["scale"] for row in rows(table_data(), "display")} == {1, 1.25, 1.5, 2}


@pytest.mark.parametrize(
    ("group", "expected"),
    [
        (
            "network",
            [
                {"round_trip_ms": 116, "throughput_kbps": 2658, "weight": 600_000},
                {"round_trip_ms": 90, "throughput_kbps": 1456, "weight": 400_000},
            ],
        ),
        ("color_scheme", [{"color_scheme": "light", "weight": 700_000}, {"color_scheme": "dark", "weight": 300_000}]),
        ("reduced_motion", [{"reduced_motion": "no-preference", "weight": 1_000_000}]),
        ("reduced_transparency", [{"reduced_transparency": "no-preference", "weight": 1_000_000}]),
    ],
)
def test_the_owners_weights_go_where_no_public_source_has_them(group: str, expected: list[dict[str, Any]]) -> None:
    data = table_data()

    assert rows(data, group) == expected
    assert data["groups"][group]["source"].startswith("The owner of mitto")


def test_the_table_credits_each_source_and_says_its_licence() -> None:
    data = table_data()

    assert data["licence"] == "CC-BY-SA-3.0"
    for source in (STATISTICS.windows_versions, STATISTICS.css_displays, STATISTICS.pixel_displays, STATISTICS.memory):
        assert f"{source.credit}, {source.url}, under CC BY-SA 3.0" in data["credits"]
