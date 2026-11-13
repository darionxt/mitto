"""Tests of a Machine: what Chrome can read, what a draw from the table gives, and the client hints worked out from it.

They need no native library. What a Session sends of its Machine is in tests/test_client_hints.py.
"""

import dataclasses
import json
import re
import subprocess
import sys
from collections.abc import Mapping
from fractions import Fraction
from typing import Any

import pytest

from mitto import Machine
from mitto._machine import (
    MACHINE_HINTS,
    client_hints,
    draw_noise_salts,
    effective_connection_type,
    load_table,
    machine_from_saved,
    rounded_downlink,
    rounded_rtt,
    windows_of,
    windows_time_zones,
)
from mitto._persona import WINDOWS_PERSONAS, load_persona
from tools.capture.machine_forms import FORMS, SCRIPT_FORMS
from tools.capture.normal_form import HINTS_FROM_THE_MACHINE
from tools.capture.script_normal_form import (
    MACHINE_PLACES,
)
from tools.parity.script_machine import PAGE_READS, WORKED_OUT_IN_A_PAGE

TABLE = load_table()
GROUPS = TABLE["groups"]

# The groups of values that a draw takes from one row together.
WORK_AREA = ("work_area_x", "work_area_y", "work_area_width", "work_area_height")
WINDOW = ("window_x", "window_y", "window_width", "window_height")
DISPLAY = (
    "display_width",
    "display_height",
    "scale",
    *WORK_AREA,
    "window_state",
    *WINDOW,
    "viewport_width",
    "viewport_height",
)
NETWORK = ("round_trip_ms", "throughput_kbps")

# What a Machine of mitto 0.2 held of its display, which a caller can give alone for a display the table does not have.
DISPLAY_OF_0_2 = ("display_width", "display_height", "scale", "viewport_width", "viewport_height")

# What mitto 0.2 saved of a Machine.
FIELDS_OF_0_2 = (
    "memory",
    *DISPLAY_OF_0_2,
    "round_trip_ms",
    "throughput_kbps",
    "color_scheme",
    "reduced_motion",
    "reduced_transparency",
    "platform_version",
    "seed",
)

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
    return {name: value for name, value in row.items() if name not in {"weight", "windows", "locale"}}


def at_scale(scale: float, viewport_width: int, viewport_height: int) -> Machine:
    """Return a Machine with a 1920 by 1080 display at a scale, with a window around a viewport."""
    return Machine.draw(
        seed=0,
        display_width=1920,
        display_height=1080,
        scale=scale,
        viewport_width=viewport_width,
        viewport_height=viewport_height,
    )


def saved_by_0_2(machine: Machine) -> dict[str, Any]:
    """Return what mitto 0.2 wrote of a Machine into a file of cookies."""
    return {name: value for name, value in dataclasses.asdict(machine).items() if name in FIELDS_OF_0_2}


def saved_without_a_refresh_rate(machine: Machine) -> dict[str, Any]:
    """Return what a build of mitto wrote of a Machine before a Machine held its refresh rate."""
    return {name: value for name, value in dataclasses.asdict(machine).items() if name != "refresh_rate"}


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
    machine = at_scale(scale, 800, 400)

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
    machine = at_scale(scale, 300, 200)

    assert FORMS["dpr"].fits(client_hints(machine)["dpr"])


@pytest.mark.parametrize(
    "platform_version",
    ["1.0.0", "2.0.0", "3.0.0", "4.0.0", "5.0.0", "6.0.0", "7.0.0", "8.0.0", "10.0.0", "14.0.0", "15.0.0", "19.0.0"],
)
def test_every_platform_version_a_machine_takes_has_the_form_chrome_writes(platform_version: str) -> None:
    machine = dataclasses.replace(Machine.named("mitto-0.1"), platform_version=platform_version)

    assert FORMS["sec-ch-ua-platform-version"].fits(client_hints(machine)["sec-ch-ua-platform-version"])


# What a page reads of a Machine, which a Page gets in its start message (SessionState._page_start()), held to the forms
# that the Normal Form of a Script Fingerprint checks real Chrome's values against, in tools/capture/machine_forms.py.
# tools/parity/script_machine.py holds the table, which the Parity Checks of a Script Fingerprint also hold a Page to.


def test_each_place_of_the_machine_is_read_from_a_machine_or_worked_out_in_a_page() -> None:
    assert set(PAGE_READS).isdisjoint(WORKED_OUT_IN_A_PAGE)
    assert set(PAGE_READS) | WORKED_OUT_IN_A_PAGE == set(MACHINE_PLACES)


@pytest.mark.parametrize("seed", SEEDS)
def test_each_value_a_drawn_machine_gives_a_page_has_the_form_chrome_gives(seed: int) -> None:
    machine = Machine.draw(seed=seed)

    misfits = [str(place) for place, read in PAGE_READS.items() if not SCRIPT_FORMS[place].fits(read(machine))]

    assert misfits == []


@pytest.mark.parametrize(
    "changes",
    [
        {"contrast_theme": "night-sky", "color_scheme": "dark", "platform_version": "19.0.0"},
        {"inverted_colors": "inverted", "touch_points": 10, "processors": 64},
        {"reduced_motion": "reduce", "reduced_transparency": "reduce", "round_trip_ms": 3000, "throughput_kbps": 0},
    ],
)
def test_the_values_of_a_machine_that_no_draw_gives_have_the_forms_chrome_gives(changes: dict[str, Any]) -> None:
    machine = Machine.draw(seed=1, **changes)

    misfits = [str(place) for place, read in PAGE_READS.items() if not SCRIPT_FORMS[place].fits(read(machine))]

    assert misfits == []


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


@pytest.mark.parametrize("row", GROUPS["display"]["rows"], ids=str)
def test_the_work_area_is_the_display_less_a_taskbar_at_its_bottom(row: dict[str, Any]) -> None:
    width, height = work_area(row)

    assert tuple(row[name] for name in WORK_AREA) == (0, 0, width, height)


@pytest.mark.parametrize(
    "row", [row for row in GROUPS["display"]["rows"] if row["window_state"] == "maximised"], ids=str
)
def test_a_maximised_window_fills_the_work_area_of_its_windows_less_chromes_bars(row: dict[str, Any]) -> None:
    width, height = work_area(row)

    assert tuple(row[name] for name in WINDOW) == (0, 0, width, height)
    assert (row["viewport_width"], row["viewport_height"]) == (width, height - MAXIMISED_BARS[row["scale"]])


@pytest.mark.parametrize("row", [row for row in GROUPS["display"]["rows"] if row["window_state"] == "normal"], ids=str)
def test_a_normal_window_is_chromes_first_window_less_its_borders(row: dict[str, Any]) -> None:
    width, height = work_area(row)
    if row["display_width"] / row["display_height"] >= 1.6 and width > 1600:
        window_width = int(width / 2 - 15)
    else:
        window_width = min(int(width) - 20, 1050)

    assert tuple(row[name] for name in WINDOW) == (10, 10, window_width, height - 20)
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
        if (row["display_width"], row["display_height"], row["scale"], row["windows"], row["window_state"])
        == (*display, windows, "normal")
    ]

    assert rows == [viewport]


def test_seven_in_ten_windows_are_maximised_and_the_rest_chromes_first_window() -> None:
    weights = {"maximised": 0, "normal": 0}
    for row in GROUPS["display"]["rows"]:
        if row["windows"] == 11:
            weights[row["window_state"]] += row["weight"]

    assert weights == {"maximised": pytest.approx(700_000, abs=50), "normal": pytest.approx(300_000, abs=50)}


@pytest.mark.parametrize("row", GROUPS["platform_version"]["rows"], ids=str)
def test_each_platform_version_of_the_table_is_of_its_windows_and_has_displays(row: dict[str, Any]) -> None:
    assert windows_of(row["platform_version"]) == row["windows"]
    assert any(display["windows"] == row["windows"] for display in GROUPS["display"]["rows"])


def test_each_windows_has_the_same_displays_with_the_same_weights() -> None:
    displays = {
        windows: [
            {name: row[name] for name in ("window_state", "display_width", "display_height", "scale", "weight")}
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
    assert {"processors": machine.processors} in [without_weight(row) for row in GROUPS["processors"]["rows"]]
    assert {"time_zone": machine.time_zone} in [without_weight(row) for row in GROUPS["time_zone"]["rows"]]


def test_draws_spread_over_the_table() -> None:
    machines = [Machine.draw(seed=seed) for seed in SEEDS]

    assert len({machine.platform_version for machine in machines}) == 2
    assert len({(machine.display_width, machine.display_height, machine.scale) for machine in machines}) > 10
    assert len({machine.memory for machine in machines}) == 5
    assert {machine.color_scheme for machine in machines} == {"light", "dark"}
    assert {(machine.audio_input, machine.video_input) for machine in machines} == {
        (True, True),
        (True, False),
        (False, True),
        (False, False),
    }
    assert len({tuple(values_of(machine).values()) for machine in machines}) > 100


def test_the_same_seed_gives_the_same_machine() -> None:
    assert Machine.draw(seed=7) == Machine.draw(seed=7)
    assert Machine.draw(seed=7).seed == 7


def test_the_salts_of_the_noise_come_from_the_seed_and_are_ones_that_chrome_draws() -> None:
    salts = [draw_noise_salts(Machine.draw(seed=seed)) for seed in range(200)]

    assert draw_noise_salts(Machine.draw(seed=7)) == draw_noise_salts(Machine.draw(seed=7, memory=32))
    assert {one.browser for one in salts} == set(range(1, 22))
    assert len({one.renderer_key for one in salts}) == 200
    assert all(re.fullmatch(r"[0-9a-f]{64}", one.renderer_key) for one in salts)


def test_a_machine_holds_no_salt_so_that_a_saved_one_saves_none() -> None:
    assert not any("salt" in field.name for field in dataclasses.fields(Machine))


def test_a_machine_with_no_seed_gets_new_salts_each_time() -> None:
    machine = Machine.named("mitto-0.1")
    salts = [draw_noise_salts(machine) for _ in range(50)]

    assert all(1 <= one.browser <= 21 for one in salts)
    assert len({one.renderer_key for one in salts}) == 50
    assert len({one.browser for one in salts}) > 1


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
        {"window_state": "normal"},
        {"window_width": 945},
        dict(zip(DISPLAY_OF_0_2, (2560, 1080, 1, 2560, 945), strict=True)),
        {"time_zone": "Europe/Berlin"},
        {"time_zone": "UTC"},
        {"processors": 6},
        {"touch_points": 10},
        {"contrast_theme": "desert"},
        {"contrast_theme": "high-contrast-black"},
        {"inverted_colors": "inverted"},
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


def test_a_contrast_theme_draws_its_windows_and_its_colour_scheme() -> None:
    windows_11 = [Machine.draw(seed=seed, contrast_theme="dusk") for seed in range(30)]
    windows_10 = [Machine.draw(seed=seed, contrast_theme="high-contrast-white") for seed in range(30)]

    assert {(machine.platform_version, machine.color_scheme) for machine in windows_11} == {("19.0.0", "dark")}
    assert {(machine.platform_version, machine.color_scheme) for machine in windows_10} == {("10.0.0", "light")}


@pytest.mark.parametrize(
    ("fixed", "message"),
    [
        (
            {"contrast_theme": "aquatic", "platform_version": "10.0.0"},
            "'aquatic' is one of Windows 11, not of Windows 10",
        ),
        ({"contrast_theme": "high-contrast-1", "platform_version": "19.0.0"}, "one of Windows 10, not of Windows 11"),
        ({"contrast_theme": "desert", "color_scheme": "dark"}, "gives the colour scheme 'light', not 'dark'"),
    ],
    ids=str,
)
def test_a_contrast_theme_refuses_what_does_not_go_with_it(fixed: dict[str, Any], message: str) -> None:
    with pytest.raises(ValueError, match=re.escape(message)):
        Machine.draw(seed=1, **fixed)


def test_a_display_that_the_table_does_not_have_gets_a_window_around_its_viewport() -> None:
    display: dict[str, Any] = dict(zip(DISPLAY_OF_0_2, (1700, 1000, 1.75, 900, 500), strict=True))
    machine = Machine.draw(seed=4, platform_version="19.0.0", **display)

    # 1700 by 1000 at 175% is 972 by 572, less the taskbar of 48, and Chrome's borders as read at 150%.
    assert tuple(getattr(machine, name) for name in WORK_AREA) == (0, 0, 972, 524)
    assert machine.window_state == "normal"
    assert tuple(getattr(machine, name) for name in WINDOW) == (10, 10, 915, 594)


def test_a_display_that_the_table_does_not_have_with_a_maximised_viewport_gets_a_maximised_window() -> None:
    display: dict[str, Any] = dict(zip(DISPLAY_OF_0_2, (1920, 1080, 1.75, 1098, 483), strict=True))
    machine = Machine.draw(seed=4, platform_version="19.0.0", **display)

    assert machine.window_state == "maximised"
    # 1920 by 1080 at 175% is 1098 by 618, less the taskbar of 48, and Chrome's bars of 87.
    assert tuple(getattr(machine, name) for name in WINDOW) == (0, 0, 1098, 570)


# A saved Machine


@pytest.mark.parametrize("seed", range(50))
def test_a_machine_that_mitto_0_2_saved_loads_as_the_machine_it_was_drawn_as(seed: int) -> None:
    machine = Machine.draw(seed=seed)

    assert machine_from_saved(saved_by_0_2(machine)) == machine


def test_a_machine_that_mitto_0_2_saved_keeps_each_value_it_has() -> None:
    saved = saved_by_0_2(Machine.draw(seed=8, memory=2, color_scheme="dark", platform_version="10.0.0"))

    assert {
        name: value for name, value in dataclasses.asdict(machine_from_saved(saved)).items() if name in saved
    } == saved


def test_a_machine_with_no_seed_that_mitto_0_2_saved_loads_the_same_each_time() -> None:
    saved = {**saved_by_0_2(Machine.draw(seed=9)), "seed": None}

    assert machine_from_saved(saved) == machine_from_saved(saved)
    assert machine_from_saved(saved).seed is None


@pytest.mark.parametrize("seed", range(50))
def test_a_machine_saved_without_a_refresh_rate_draws_it_from_its_seed(seed: int) -> None:
    machine = Machine.draw(seed=seed)

    assert machine_from_saved(saved_without_a_refresh_rate(machine)) == machine


def test_a_machine_with_no_seed_saved_without_a_refresh_rate_loads_the_same_each_time() -> None:
    saved = {**saved_without_a_refresh_rate(Machine.draw(seed=9)), "seed": None}
    loaded = machine_from_saved(saved)

    assert loaded == machine_from_saved(saved)
    assert loaded.seed is None
    assert {name: value for name, value in dataclasses.asdict(loaded).items() if name in saved} == saved
    assert {"refresh_rate": loaded.refresh_rate} in [without_weight(row) for row in GROUPS["refresh_rate"]["rows"]]


def test_a_saved_machine_loads_the_same_in_another_process() -> None:
    saved = [saved_by_0_2(Machine.draw(seed=seed)) for seed in range(5)]
    saved.append({**saved_by_0_2(Machine.draw(seed=5)), "seed": None})
    saved.append({**dict(zip(DISPLAY_OF_0_2, (1700, 1000, 1.75, 900, 500), strict=True)), **saved[0], "seed": None})
    saved.extend(saved_without_a_refresh_rate(Machine.draw(seed=seed)) for seed in range(6, 11))
    saved.append({**saved_without_a_refresh_rate(Machine.draw(seed=11)), "seed": None})
    script = (
        "import dataclasses, json, sys\n"
        "from mitto._machine import machine_from_saved\n"
        "print(json.dumps([dataclasses.asdict(machine_from_saved(s)) for s in json.loads(sys.stdin.read())]))\n"
    )
    # A new process, whose hash seed is another, as Python draws one for each.
    found = subprocess.run(
        [sys.executable, "-c", script], input=json.dumps(saved), capture_output=True, text=True, check=True
    )

    assert json.loads(found.stdout) == [dataclasses.asdict(machine_from_saved(one)) for one in saved]


def test_a_saved_machine_with_every_field_loads_as_it_is() -> None:
    machine = dataclasses.replace(Machine.draw(seed=10), time_zone="Asia/Tokyo", processors=6, touch_points=10)

    assert machine_from_saved(dataclasses.asdict(machine)) == machine


@pytest.mark.parametrize(
    "saved",
    [
        {name: value for name, value in dataclasses.asdict(Machine.named("mitto-0.1")).items() if name != "memory"},
        {**dataclasses.asdict(Machine.named("mitto-0.1")), "window": 1},
    ],
    ids=["without-memory", "with-an-unknown-field"],
)
def test_a_saved_machine_without_a_field_of_mitto_0_2_or_with_another_is_refused(saved: dict[str, Any]) -> None:
    with pytest.raises(ValueError, match="a saved Machine holds"):
        machine_from_saved(saved)


# What Chrome on Windows can give a page


def test_each_time_zone_of_the_table_is_one_that_chrome_on_windows_gives_a_page() -> None:
    zones = windows_time_zones()

    assert {row["time_zone"] for row in GROUPS["time_zone"]["rows"]} <= zones
    assert {"America/New_York", "Europe/Berlin", "Asia/Calcutta", "Etc/GMT+12", "UTC"} <= zones
    # Other zones of a region, which Chrome never takes, and names that a page never reads.
    assert not {"America/Detroit", "Asia/Kolkata", "Europe/Kyiv", "Etc/UTC", "Etc/GMT"} & zones


def test_each_locale_of_a_persona_has_a_time_zone() -> None:
    locales = {load_persona(name).locale for name in WINDOWS_PERSONAS.values()}

    assert locales == {"en-US"}
    assert locales <= {row["locale"] for row in GROUPS["time_zone"]["rows"]}


@pytest.mark.parametrize("seed", SEEDS)
def test_each_value_a_draw_gives_a_page_is_one_chrome_on_windows_gives(seed: int) -> None:
    machine = Machine.draw(seed=seed)

    assert machine.time_zone in windows_time_zones()
    # Two threads for each core, or the threads of Intel's processors with cores of two kinds.
    assert machine.processors in {4, 8, 12, 16, 20, 24, 32}
    assert machine.touch_points == 0
    assert machine.contrast_theme == "none"
    assert machine.inverted_colors == "none"
    assert machine.window_state in {"maximised", "normal"}
    # The owner's weights: something that plays sound always, a microphone in 70% and a camera in 50%.
    assert machine.audio_output


def test_the_media_devices_are_drawn_on_their_own_at_the_owners_weights() -> None:
    assert [without_weight(row) | {"weight": row["weight"]} for row in GROUPS["audio_input"]["rows"]] == [
        {"audio_input": True, "weight": 700_000},
        {"audio_input": False, "weight": 300_000},
    ]
    assert [row["weight"] for row in GROUPS["video_input"]["rows"]] == [500_000, 500_000]
    assert [without_weight(row) for row in GROUPS["audio_output"]["rows"]] == [{"audio_output": True}]
    # Fixing one leaves the others' draws as they were.
    drawn = Machine.draw(seed=12)
    camera = Machine.draw(seed=12, video_input=not drawn.video_input)
    assert (camera.audio_input, camera.memory) == (drawn.audio_input, drawn.memory)


def test_draws_spread_over_the_processors() -> None:
    assert {Machine.draw(seed=seed).processors for seed in SEEDS} == {4, 8, 12, 16, 20, 24, 32}


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
        ("time_zone", "America/Detroit", "time_zone has to be a time zone that Chrome on Windows gives"),
        ("time_zone", "Etc/UTC", "time_zone has to be a time zone that Chrome on Windows gives"),
        ("processors", 0, "processors has to be from 1 to 64"),
        ("processors", 65, "processors has to be from 1 to 64"),
        ("touch_points", -1, "touch_points has to be from 0 to 256"),
        ("touch_points", 257, "touch_points has to be from 0 to 256"),
        ("refresh_rate", 23, "refresh_rate has to be from 24 to 1000"),
        ("refresh_rate", 1001, "refresh_rate has to be from 24 to 1000"),
        ("window_state", "first", "window_state has to be 'maximised' or 'normal'"),
        ("contrast_theme", "high-contrast", "contrast_theme has to be 'none' or 'aquatic'"),
        ("inverted_colors", "reduce", "inverted_colors has to be 'none' or 'inverted'"),
        ("work_area_x", -1, "work_area_x has to be at least 0"),
        ("window_width", 0, "window_width has to be at least 1"),
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
    ("changes", "message"),
    [
        ({"work_area_height": 1081}, "the work area (0, 0, 1920, 1081) does not fit on a display of 1920 by 1080"),
        ({"work_area_x": 1, "window_x": 1}, "the work area (1, 0, 1920, 1032) does not fit"),
        ({"window_height": 1000}, "a maximised window is the work area (0, 0, 1920, 1032), not (0, 0, 1920, 1000)"),
        (
            {"window_state": "normal", "window_width": 1000},
            "the viewport of 1920 by 945 does not fit in a window of 1000 by 1032",
        ),
        ({"contrast_theme": "aquatic"}, "the contrast theme 'aquatic' gives the colour scheme 'dark', not 'light'"),
    ],
    ids=str,
)
def test_a_desktop_that_does_not_fit_together_is_refused(changes: dict[str, Any], message: str) -> None:
    with pytest.raises(ValueError, match=re.escape(message)):
        dataclasses.replace(Machine.named("mitto-0.1"), **changes)


@pytest.mark.parametrize(
    ("name", "value", "message"),
    [
        ("memory", 8.0, "memory has to be a whole number, not float"),
        ("memory", True, "memory has to be a whole number, not bool"),
        ("scale", "1", "scale has to be a number, not str"),
        ("scale", True, "scale has to be a number, not bool"),
        ("viewport_width", "1920", "viewport_width has to be a whole number, not str"),
        ("refresh_rate", 59.94, "refresh_rate has to be a whole number, not float"),
        ("color_scheme", None, "color_scheme has to be text, not NoneType"),
        ("platform_version", 19, "platform_version has to be text, not int"),
        ("audio_input", 1, "audio_input has to be True or False, not int"),
        ("video_input", "yes", "video_input has to be True or False, not str"),
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
    display: dict[str, Any] = dict(zip(DISPLAY_OF_0_2, (1093, 615, 1.25, 875, 400), strict=True))
    machine = Machine.draw(seed=4, **display)

    assert (machine.display_width, machine.viewport_width) == (1093, 875)


def test_a_viewport_can_fill_a_display_whose_size_in_device_independent_pixels_is_not_whole() -> None:
    machine = dataclasses.replace(
        Machine.named("mitto-0.1"),
        display_width=1366,
        display_height=768,
        scale=1.25,
        work_area_width=1093,
        work_area_height=567,
        window_state="normal",
        window_width=1093,
        window_height=615,
        viewport_width=1093,
        viewport_height=615,
    )

    assert machine.viewport_width == 1093
