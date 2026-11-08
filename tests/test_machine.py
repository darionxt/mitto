"""Tests of a Machine: what Chrome can read, what a draw from the table gives, and the client hints worked out from it.

They need no native library. What a Session sends of its Machine is in tests/test_client_hints.py.
"""

import dataclasses
import re
from collections.abc import Mapping
from fractions import Fraction
from typing import Any

import pytest

from mitto import Machine
from mitto._machine import (
    MACHINE_HINTS,
    client_hints,
    effective_connection_type,
    load_table,
    rounded_downlink,
    rounded_rtt,
    windows_of,
)
from tools.capture.machine_forms import FORMS
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE

TABLE = load_table()
GROUPS = TABLE["groups"]

# The groups of values that a draw takes from one row together.
DISPLAY = ("display_width", "display_height", "scale", "viewport_width", "viewport_height")
NETWORK = ("round_trip_ms", "throughput_kbps")

# The window, in device-independent pixels, as read on Windows 11 at each scale and on Windows 10 at 100%: the taskbar of
# each Windows, Chrome's bars in a maximised window, and the bars and top border, and the side borders, of Chrome's first
# window on a new profile, which is not maximised.
TASKBAR_HEIGHTS = {10: 40, 11: 48}
MAXIMISED_BARS = {1: 87, 1.25: 86, 1.5: 87, 2: 87}
FIRST_WINDOW_TOP = {1: 95, 1.25: 95, 1.5: 94, 2: 93}
FIRST_WINDOW_SIDES = {1: 16, 1.25: 14, 1.5: 15, 2: 13}

SEEDS = range(500)

SAME_AS = {
    "sec-ch-device-memory": "device-memory",
    "sec-ch-dpr": "dpr",
    "sec-ch-viewport-width": "viewport-width",
}

MITTO_0_1 = {
    "device-memory": "8",
    "sec-ch-device-memory": "8",
    "dpr": "1",
    "sec-ch-dpr": "1",
    "viewport-width": "1920",
    "sec-ch-viewport-width": "1920",
    "sec-ch-viewport-height": "945",
    "rtt": "50",
    "downlink": "10",
    "ect": "4g",
    "sec-ch-prefers-color-scheme": "light",
    "sec-ch-prefers-reduced-motion": "no-preference",
    "sec-ch-prefers-reduced-transparency": "no-preference",
    "sec-ch-ua-platform-version": '"19.0.0"',
}


def values_of(machine: Machine) -> dict[str, Any]:
    return {name: value for name, value in dataclasses.asdict(machine).items() if name != "seed"}


def without_weight(row: Mapping[str, Any]) -> dict[str, Any]:
    return {name: value for name, value in row.items() if name not in {"weight", "windows", "window"}}


def work_area(row: Mapping[str, Any]) -> tuple[Fraction, Fraction]:
    scale = Fraction(row["scale"])
    return row["display_width"] / scale, row["display_height"] / scale - TASKBAR_HEIGHTS[row["windows"]]


# The rounding, against the cases of content/browser/client_hints/client_hints_unittest.cc, which has noise as well.


@pytest.mark.parametrize("round_trip_ms", [1023, 6787, 12, 1193, 98, 2750, 2789, 6023])
def test_rtt_is_a_multiple_of_50_of_at_most_3000_near_the_round_trip(round_trip_ms: int) -> None:
    rtt = rounded_rtt(round_trip_ms)

    assert rtt % 50 == 0
    assert rtt <= 3000
    assert abs(rtt - min(round_trip_ms, 3000)) <= 25


@pytest.mark.parametrize("throughput_kbps", [102, 12, 2102, 12102, 98, 1023, 1193, 7523, 9999, 100102])
def test_downlink_is_a_multiple_of_50_kbps_of_at_most_10_mbps_near_the_throughput(throughput_kbps: int) -> None:
    downlink = rounded_downlink(throughput_kbps)

    assert round(downlink * 1000) % 50 == 0
    assert downlink <= 10
    assert abs(downlink * 1000 - min(throughput_kbps, 10_000)) <= 25


@pytest.mark.parametrize(
    ("round_trip_ms", "rtt"), [(0, 0), (24, 0), (25, 50), (90, 100), (116, 100), (125, 150), (2999, 3000), (3100, 3000)]
)
def test_rtt_goes_to_the_nearer_multiple_of_50_and_up_from_halfway(round_trip_ms: int, rtt: int) -> None:
    assert rounded_rtt(round_trip_ms) == rtt


@pytest.mark.parametrize(
    ("throughput_kbps", "downlink"),
    [(0, "0"), (25, "0.05"), (1456, "1.45"), (2658, "2.65"), (9975, "10"), (10_000, "10"), (20_000, "10")],
)
def test_downlink_is_written_as_chrome_writes_a_number(throughput_kbps: int, downlink: str) -> None:
    machine = dataclasses.replace(Machine.named("mitto-0.1"), throughput_kbps=throughput_kbps)

    assert client_hints(machine)["downlink"] == downlink


@pytest.mark.parametrize(
    ("round_trip_ms", "ect"),
    [
        (0, "4g"),
        (271, "4g"),
        (272, "3g"),
        (1419, "3g"),
        (1420, "2g"),
        (2009, "2g"),
        (2010, "slow-2g"),
        (9000, "slow-2g"),
    ],
)
def test_ect_follows_the_round_trip_by_the_thresholds_of_the_estimator(round_trip_ms: int, ect: str) -> None:
    assert effective_connection_type(round_trip_ms) == ect


@pytest.mark.parametrize(
    ("scale", "dpr"), [(1, "1"), (1.0, "1"), (1.25, "1.25"), (1.5, "1.5"), (2, "2"), (2.25, "2.25")]
)
def test_dpr_is_the_scale_as_chrome_writes_a_number(scale: float, dpr: str) -> None:
    machine = dataclasses.replace(Machine.named("mitto-0.1"), scale=scale, viewport_width=800, viewport_height=400)

    assert client_hints(machine)["dpr"] == client_hints(machine)["sec-ch-dpr"] == dpr


# The forms that the normal form checks real Chrome's hidden values against, in tools/capture/machine_forms.py.


def test_every_rtt_downlink_and_ect_has_the_form_chrome_writes() -> None:
    for estimate in range(0, 11_000, 5):
        machine = dataclasses.replace(Machine.named("mitto-0.1"), round_trip_ms=estimate, throughput_kbps=estimate)
        hints = client_hints(machine)

        for name in ("rtt", "downlink", "ect"):
            assert FORMS[name].fits(hints[name]), (name, estimate)


@pytest.mark.parametrize("scale", [1, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 3, 3.5, 4, 4.5, 5])
def test_every_scale_windows_offers_has_the_form_chrome_writes(scale: float) -> None:
    machine = dataclasses.replace(Machine.named("mitto-0.1"), scale=scale, viewport_width=300, viewport_height=200)

    assert FORMS["dpr"].fits(client_hints(machine)["dpr"])


@pytest.mark.parametrize(
    "platform_version",
    ["1.0.0", "2.0.0", "3.0.0", "4.0.0", "5.0.0", "6.0.0", "7.0.0", "8.0.0", "10.0.0", "14.0.0", "15.0.0", "19.0.0"],
)
def test_every_platform_version_a_machine_takes_has_the_form_chrome_writes(platform_version: str) -> None:
    machine = dataclasses.replace(Machine.named("mitto-0.1"), platform_version=platform_version)

    assert FORMS["sec-ch-ua-platform-version"].fits(client_hints(machine)["sec-ch-ua-platform-version"])


# The hints


def test_a_machine_gives_each_hint_that_follows_the_machine() -> None:
    assert set(client_hints(Machine.draw(seed=1))) == MACHINE_HINTS
    assert HINTS_FROM_THE_MACHINE == MACHINE_HINTS


def test_the_machine_of_mitto_0_1_says_what_every_session_of_mitto_0_1_said() -> None:
    assert client_hints(Machine.named("mitto-0.1")) == MITTO_0_1
    assert Machine.named("mitto-0.1").seed is None


def test_another_name_is_no_machine() -> None:
    with pytest.raises(ValueError, match=r"only 'mitto-0\.1'"):
        Machine.named("mitto-0.2")
    with pytest.raises(TypeError, match="name has to be text"):
        Machine.named(1)  # pyright: ignore[reportArgumentType]


# What a draw gives


@pytest.mark.parametrize("group", list(GROUPS))
def test_each_row_of_the_table_has_a_weight_and_each_group_has_rows(group: str) -> None:
    rows = GROUPS[group]["rows"]

    assert rows
    assert all(isinstance(row["weight"], int) and row["weight"] > 0 for row in rows)
    assert GROUPS[group]["source"]


def test_the_table_has_its_licence_and_its_credits() -> None:
    assert TABLE["licence"] == "CC-BY-SA-3.0"
    credits = " ".join(TABLE["credits"])
    assert "StatCounter" in credits
    assert "Firefox Public Data Report" in credits
    assert "https://creativecommons.org/licenses/by-sa/3.0/" in credits


@pytest.mark.parametrize("row", [row for row in GROUPS["display"]["rows"] if row["window"] == "maximised"], ids=str)
def test_a_maximised_window_fills_the_work_area_of_its_windows_less_chromes_bars(row: dict[str, Any]) -> None:
    width, height = work_area(row)

    assert (row["viewport_width"], row["viewport_height"]) == (width, height - MAXIMISED_BARS[row["scale"]])


@pytest.mark.parametrize("row", [row for row in GROUPS["display"]["rows"] if row["window"] == "first"], ids=str)
def test_a_first_window_is_chromes_default_size_less_its_borders(row: dict[str, Any]) -> None:
    width, height = work_area(row)
    if row["display_width"] / row["display_height"] >= 1.6 and width > 1600:
        window_width = int(width / 2 - 15)
    else:
        window_width = min(int(width) - 20, 1050)

    assert row["viewport_width"] == window_width - FIRST_WINDOW_SIDES[row["scale"]]
    assert row["viewport_height"] == height - 20 - FIRST_WINDOW_TOP[row["scale"]]


@pytest.mark.parametrize(
    ("display", "windows", "viewport"),
    [
        ((1920, 1080, 1), 11, (929, 917)),
        ((1920, 1080, 1), 10, (929, 925)),
        ((1920, 1080, 1.25), 11, (1036, 701)),
        ((1366, 768, 1), 11, (1034, 605)),
        ((2560, 1440, 1), 11, (1249, 1277)),
    ],
    ids=str,
)
def test_a_first_window_has_the_viewport_that_chrome_gives_it(
    display: tuple[int, int, float], windows: int, viewport: tuple[int, int]
) -> None:
    rows = [
        (row["viewport_width"], row["viewport_height"])
        for row in GROUPS["display"]["rows"]
        if (row["display_width"], row["display_height"], row["scale"], row["windows"], row["window"])
        == (*display, windows, "first")
    ]

    assert rows == [viewport]


def test_seven_in_ten_windows_are_maximised_and_the_rest_chromes_first_window() -> None:
    weights = {"maximised": 0, "first": 0}
    for row in GROUPS["display"]["rows"]:
        if row["windows"] == 11:
            weights[row["window"]] += row["weight"]

    assert weights == {"maximised": pytest.approx(700_000, abs=50), "first": pytest.approx(300_000, abs=50)}


@pytest.mark.parametrize("row", GROUPS["platform_version"]["rows"], ids=str)
def test_each_platform_version_of_the_table_is_of_its_windows_and_has_displays(row: dict[str, Any]) -> None:
    assert windows_of(row["platform_version"]) == row["windows"]
    assert any(display["windows"] == row["windows"] for display in GROUPS["display"]["rows"])


def test_each_windows_has_the_same_displays_with_the_same_weights() -> None:
    displays = {
        windows: [
            {name: row[name] for name in ("window", "display_width", "display_height", "scale", "weight")}
            for row in GROUPS["display"]["rows"]
            if row["windows"] == windows
        ]
        for windows in (10, 11)
    }

    assert displays[10] == displays[11]


@pytest.mark.parametrize("seed", SEEDS)
def test_each_value_a_draw_gives_is_one_chrome_sends(seed: int) -> None:
    hints = client_hints(Machine.draw(seed=seed))

    for name, value in hints.items():
        assert FORMS[name].fits(value), name
    for name, same_as in SAME_AS.items():
        assert hints[name] == hints[same_as]


@pytest.mark.parametrize("seed", SEEDS)
def test_a_draw_keeps_to_what_goes_together(seed: int) -> None:
    machine = Machine.draw(seed=seed)
    values = values_of(machine)
    windows = windows_of(machine.platform_version)

    assert {name: values[name] for name in DISPLAY} in [
        without_weight(row) for row in GROUPS["display"]["rows"] if row["windows"] == windows
    ]
    assert {name: values[name] for name in NETWORK} in [without_weight(row) for row in GROUPS["network"]["rows"]]


def test_draws_spread_over_the_table() -> None:
    machines = [Machine.draw(seed=seed) for seed in SEEDS]

    assert len({machine.platform_version for machine in machines}) == 2
    assert len({(machine.display_width, machine.display_height, machine.scale) for machine in machines}) > 10
    assert len({machine.memory for machine in machines}) == 5
    assert {machine.color_scheme for machine in machines} == {"light", "dark"}
    assert len({tuple(values_of(machine).values()) for machine in machines}) > 100


def test_the_same_seed_gives_the_same_machine() -> None:
    assert Machine.draw(seed=7) == Machine.draw(seed=7)
    assert Machine.draw(seed=7).seed == 7


def test_a_machine_drawn_with_no_seed_keeps_the_seed_that_draws_it_again() -> None:
    machine = Machine.draw()

    assert machine.seed is not None
    assert Machine.draw(seed=machine.seed) == machine
    assert Machine.draw() != machine


@pytest.mark.parametrize(
    "fixed",
    [
        {"memory": 32},
        {"color_scheme": "dark"},
        {"platform_version": "10.0.0"},
        {"platform_version": "15.0.0"},
        {"round_trip_ms": 90, "throughput_kbps": 1456},
        {"round_trip_ms": 400, "throughput_kbps": 700},
        {"reduced_motion": "reduce", "reduced_transparency": "reduce"},
        {"display_width": 2560, "display_height": 1440},
        {"scale": 1.25},
        {"viewport_width": 1536},
        dict(zip(DISPLAY, (2560, 1080, 1, 2560, 945), strict=True)),
    ],
    ids=str,
)
def test_a_value_given_is_kept_and_the_rest_drawn_to_fit_it(fixed: dict[str, Any]) -> None:
    machine = Machine.draw(seed=3, **fixed)

    assert {name: values_of(machine)[name] for name in fixed} == fixed
    assert machine.seed == 3


def test_a_value_given_leaves_the_draw_of_other_groups_as_it_was() -> None:
    drawn = values_of(Machine.draw(seed=11))
    with_memory = values_of(Machine.draw(seed=11, memory=2 if drawn["memory"] != 2 else 4))

    assert {name: value for name, value in with_memory.items() if name != "memory"} == {
        name: value for name, value in drawn.items() if name != "memory"
    }


def test_a_viewport_of_one_windows_draws_that_windows() -> None:
    machine = Machine.draw(seed=5, display_width=1920, display_height=1080, scale=1, viewport_height=953)

    assert machine.platform_version == "10.0.0"


@pytest.mark.parametrize(
    ("fixed", "message"),
    [
        ({"viewport_width": 1281}, "viewport_width=1281"),
        ({"round_trip_ms": 400}, "round_trip_ms=400"),
        ({"platform_version": "19.0.0", "viewport_height": 953}, "on Windows 11"),
    ],
    ids=str,
)
def test_a_value_of_a_group_that_no_row_has_needs_the_rest_of_the_group(fixed: dict[str, Any], message: str) -> None:
    with pytest.raises(ValueError, match=message):
        Machine.draw(seed=3, **fixed)


# What a Machine refuses


def test_a_machine_that_a_caller_makes_has_no_seed() -> None:
    machine = Machine(**values_of(Machine.draw(seed=2)))

    assert machine.seed is None
    assert values_of(machine) == values_of(Machine.draw(seed=2))


@pytest.mark.parametrize(
    ("name", "value", "message"),
    [
        ("memory", 12, "memory has to be 2, 4, 8, 16 or 32"),
        ("memory", 64, "memory has to be 2, 4, 8, 16 or 32"),
        ("scale", 1.1, "scale has to be one that Windows offers"),
        ("scale", float("nan"), "scale has to be one that Windows offers"),
        ("display_width", 0, "display_width has to be at least 1"),
        ("viewport_height", 0, "viewport_height has to be at least 1"),
        ("round_trip_ms", -1, "round_trip_ms has to be from 0"),
        ("throughput_kbps", 2**31, "throughput_kbps has to be from 0"),
        ("color_scheme", "Dark", "color_scheme has to be 'light' or 'dark'"),
        ("reduced_motion", "none", "reduced_motion has to be 'no-preference' or 'reduce'"),
        ("reduced_transparency", "", "reduced_transparency has to be 'no-preference' or 'reduce'"),
        ("platform_version", "9.0.0", "platform_version has to be one that Windows 10 or 11 gives"),
        ("platform_version", "19.0", "platform_version has to be one that Windows 10 or 11 gives"),
    ],
    ids=str,
)
def test_a_value_that_chrome_cannot_read_is_refused(name: str, value: object, message: str) -> None:
    with pytest.raises(ValueError, match=re.escape(message)):
        dataclasses.replace(Machine.named("mitto-0.1"), **{name: value})
    given: dict[str, Any] = {name: value}
    with pytest.raises(ValueError, match=re.escape(message)):
        Machine.draw(**given)


@pytest.mark.parametrize(("width", "height"), [(1921, 945), (1920, 1081)])
def test_a_viewport_larger_than_the_display_is_refused(width: int, height: int) -> None:
    with pytest.raises(
        ValueError, match=f"the viewport of {width} by {height} does not fit on a display of 1920 by 1080"
    ):
        dataclasses.replace(Machine.named("mitto-0.1"), viewport_width=width, viewport_height=height)


@pytest.mark.parametrize(
    ("name", "value", "message"),
    [
        ("memory", 8.0, "memory has to be a whole number, not float"),
        ("memory", True, "memory has to be a whole number, not bool"),
        ("scale", "1", "scale has to be a number, not str"),
        ("scale", True, "scale has to be a number, not bool"),
        ("viewport_width", "1920", "viewport_width has to be a whole number, not str"),
        ("color_scheme", None, "color_scheme has to be text, not NoneType"),
        ("platform_version", 19, "platform_version has to be text, not int"),
        ("seed", 1.5, "seed has to be a whole number, not float"),
    ],
    ids=str,
)
def test_a_value_of_another_type_is_refused(name: str, value: object, message: str) -> None:
    with pytest.raises(TypeError, match=re.escape(message)):
        dataclasses.replace(Machine.named("mitto-0.1"), **{name: value})
    given: dict[str, Any] = {name: value}
    if value is not None:
        with pytest.raises(TypeError, match=re.escape(message)):
            Machine.draw(**given)


def test_a_whole_display_that_the_table_does_not_have_can_be_given() -> None:
    display: dict[str, Any] = dict(zip(DISPLAY, (1093, 615, 1.25, 875, 400), strict=True))
    machine = Machine.draw(seed=4, **display)

    assert (machine.display_width, machine.viewport_width) == (1093, 875)


def test_a_viewport_can_fill_a_display_whose_size_in_device_independent_pixels_is_not_whole() -> None:
    machine = dataclasses.replace(
        Machine.named("mitto-0.1"),
        display_width=1366,
        display_height=768,
        scale=1.25,
        viewport_width=1093,
        viewport_height=615,
    )

    assert machine.viewport_width == 1093
