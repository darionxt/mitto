"""Writing the table that a Session's Machine is drawn from, from public statistics, the way docs/persona.md describes.

The statistics are StatCounter's and the Firefox Public Data Report's, which are under CC BY-SA 3.0, so the table is too,
with their credits in it. Where no public source has weights, the table takes the owner's and says so. What the sources
said for the period the table is made from is in machine_statistics.json beside this, which machine_sources.py fetches
again.
"""

import itertools
import json
from collections import defaultdict
from collections.abc import Iterable, Mapping
from dataclasses import asdict, dataclass, fields
from fractions import Fraction
from pathlib import Path
from typing import Any

__all__ = [
    "STATISTICS",
    "STATISTICS_PATH",
    "TABLE_PATH",
    "Display",
    "MachineTableError",
    "Source",
    "Statistics",
    "approximated_device_memory",
    "display_shares",
    "dumps_statistics",
    "dumps_table",
    "read_statistics",
    "table_data",
    "write_table",
]

TABLE_PATH = Path(__file__).parents[2] / "src" / "mitto" / "machine_table.json"
STATISTICS_PATH = Path(__file__).with_name("machine_statistics.json")

LICENCE = "CC-BY-SA-3.0"
_LICENCE_URL = "https://creativecommons.org/licenses/by-sa/3.0/"

# The share of each group's rows is written as a whole number of parts in this many, so that a draw needs no floats.
_PARTS = 1_000_000


@dataclass(frozen=True, slots=True)
class Source:
    """What one public source says of one thing, for one period.

    Attributes:
        credit: The source, what it measures and the period, as its credit in the table says it.
        url: Where to find it.
        shares: The share in percent of each value, by the name the source gives it, such as "1920x1080" or "8GB".
    """

    credit: str
    url: str
    shares: Mapping[str, float]


@dataclass(frozen=True, slots=True)
class Statistics:
    """The public statistics that the table is made from.

    Attributes:
        windows_versions: StatCounter's shares of each Windows on desktops, such as "Win11".
        css_displays: StatCounter's shares of each display size on desktops, in CSS pixels, so with the scale applied.
        pixel_displays: Firefox's shares of each display size, in pixels.
        memory: Firefox's shares of each size of memory, in whole GB.
    """

    windows_versions: Source
    css_displays: Source
    pixel_displays: Source
    memory: Source


def read_statistics(path: Path = STATISTICS_PATH) -> Statistics:
    """Return the statistics that a file of them holds."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return Statistics(**{field.name: Source(**data[field.name]) for field in fields(Statistics)})


def dumps_statistics(statistics: Statistics) -> str:
    """Write statistics as text, with one line for each share, so that a change to a share is one line of a diff."""
    return json.dumps({"licence": LICENCE, **asdict(statistics)}, indent=2, ensure_ascii=False) + "\n"


STATISTICS = read_statistics()
"""What the sources said when they were last fetched, for the period that each credit names. Their "Other" counts for
nothing."""

_OWNER = "The owner of mitto, where no public source has weights"

# The Windows that StatCounter names, and the sec-ch-ua-platform-version that each claims: the one of its last versions,
# which Microsoft moves most users to. Older Windows cannot run Chrome 155 to 157.
_WINDOWS = {"Win11": ("19.0.0", 11), "Win10": ("10.0.0", 10)}

# The scales of Windows' display settings at which Chrome's bars were read on Windows 11, so that a viewport can be worked
# out. Windows offers more, which wait for a reading.
_SCALES = tuple(Fraction(percent, 100) for percent in (100, 125, 150, 200))

# Chrome on a desktop holds the memory between these, in GiB.
_SMALLEST_MEMORY_GIB = 2
_LARGEST_MEMORY_GIB = 32
_MIB_PER_GIB = 1024

# The owner's weights, in percent.
_COLOR_SCHEMES = {"light": 70, "dark": 30}
_REDUCED_MOTION = {"no-preference": 100}
_REDUCED_TRANSPARENCY = {"no-preference": 100}


@dataclass(frozen=True, slots=True, order=True)
class Display:
    """A display in pixels, at a scale of Windows' display settings."""

    width: int
    height: int
    scale: Fraction

    @property
    def size(self) -> tuple[int, int]:
        return (self.width, self.height)


class MachineTableError(Exception):
    """The statistics cannot make the table."""


def approximated_device_memory(physical_memory_mib: int) -> int:
    """Return the memory in GiB that Chrome on a desktop sends for this much memory: the nearer power of two, the lower one
    on a tie, held between 2 and 32 GiB. third_party/blink/common/device_memory/approximated_device_memory.cc does this.
    """
    lower = 1 << (physical_memory_mib.bit_length() - 1)
    upper = lower << 1
    nearer = lower if physical_memory_mib - lower <= upper - physical_memory_mib else upper
    return min(max(nearer // _MIB_PER_GIB, _SMALLEST_MEMORY_GIB), _LARGEST_MEMORY_GIB)


def display_shares(css_displays: Mapping[str, float], pixel_displays: Mapping[str, float]) -> dict[Display, float]:
    """Return the share of each display in pixels at each scale.

    A size in pixels has its share from `pixel_displays`. That share is split between the scales Windows offers by how
    common `css_displays` finds the size each scale gives in CSS pixels, and a size in CSS pixels that more than one display
    gives is shared out between them by their shares in pixels. A size in pixels that gives no size in `css_displays` at
    any scale is at 100%. A name that is no size, such as "Other", counts for nothing.
    """
    pixels = _sizes(pixel_displays)
    css = _sizes(css_displays)
    givers: defaultdict[tuple[int, int], list[Display]] = defaultdict(list)
    for (width, height), scale in itertools.product(pixels, _SCALES):
        in_css = (width / scale, height / scale)
        if all(side.denominator == 1 for side in in_css) and (size := (int(in_css[0]), int(in_css[1]))) in css:
            givers[size].append(Display(width, height, scale))
    found: defaultdict[tuple[int, int], dict[Fraction, float]] = defaultdict(dict)
    for size, displays in givers.items():
        total = sum(pixels[display.size] for display in displays)
        for display in displays:
            found[display.size][display.scale] = css[size] * pixels[display.size] / total
    shares: dict[Display, float] = {}
    for size, share in pixels.items():
        by_scale = found.get(size) or {Fraction(1): 1.0}
        total = sum(by_scale.values())
        for scale, weight in by_scale.items():
            shares[Display(*size, scale)] = share * weight / total
    return shares


def table_data(statistics: Statistics = STATISTICS) -> dict[str, Any]:
    """Return what the table holds, made from `statistics` and the owner's weights.

    Raises:
        MachineTableError: If a source names a value that the table cannot use, or has no share of what it needs.
    """
    return {
        "licence": LICENCE,
        "credits": [
            *(
                f"{source.credit}, {source.url}, under CC BY-SA 3.0"
                for source in (
                    statistics.windows_versions,
                    statistics.css_displays,
                    statistics.pixel_displays,
                    statistics.memory,
                )
            ),
            f"Worked into a table of weighted rows for mitto by its authors, and shared under CC BY-SA 3.0, {_LICENCE_URL}",
        ],
        "groups": {
            "platform_version": {
                "source": statistics.windows_versions.credit,
                "rows": _platform_version_rows(statistics.windows_versions.shares),
            },
            "display": {
                "source": (
                    f"{statistics.pixel_displays.credit}, split by scale by {statistics.css_displays.credit}. The window:"
                    f" {_OWNER_WINDOWS}, with the taskbar and Chrome's bars read on Windows 11 at each scale and on"
                    " Windows 10 at 100%, whose taskbar is taken to be as high at every scale"
                ),
                "rows": _display_rows(statistics),
            },
            "memory": {"source": statistics.memory.credit, "rows": _memory_rows(statistics.memory.shares)},
            "network": {
                "source": f"{_OWNER}: Chrome's starting values for Wi-Fi and Ethernet",
                "rows": _network_rows(),
            },
            "color_scheme": {"source": _OWNER, "rows": _rows("color_scheme", _COLOR_SCHEMES)},
            "reduced_motion": {"source": _OWNER, "rows": _rows("reduced_motion", _REDUCED_MOTION)},
            "reduced_transparency": {"source": _OWNER, "rows": _rows("reduced_transparency", _REDUCED_TRANSPARENCY)},
        },
    }


def dumps_table(data: Mapping[str, Any]) -> str:
    """Write what the table holds as text, with one line for each row, so that a change to a weight is one line of a diff."""
    groups = [
        f'    "{name}": {{\n'
        f'      "source": {json.dumps(group["source"])},\n'
        '      "rows": [\n' + ",\n".join(f"        {json.dumps(row)}" for row in group["rows"]) + "\n      ]\n    }"
        for name, group in data["groups"].items()
    ]
    credits = ",\n".join(f"    {json.dumps(credit)}" for credit in data["credits"])
    return (
        "{\n"
        f'  "licence": {json.dumps(data["licence"])},\n'
        f'  "credits": [\n{credits}\n  ],\n'
        '  "groups": {\n' + ",\n".join(groups) + "\n  }\n}\n"
    )


def write_table(statistics: Statistics = STATISTICS, path: Path = TABLE_PATH) -> Path:
    """Write the table from `statistics` to `path`, and return the path.

    Raises:
        MachineTableError: See table_data().
    """
    path.write_text(dumps_table(table_data(statistics)), encoding="utf-8")
    return path


def _platform_version_rows(shares: Mapping[str, float]) -> list[dict[str, Any]]:
    named = {name: share for name, share in shares.items() if name in _WINDOWS}
    if set(named) != set(_WINDOWS):
        raise MachineTableError(f"the shares of Windows have to name {' and '.join(_WINDOWS)}")
    rows = [
        {"platform_version": _WINDOWS[name][0], "windows": _WINDOWS[name][1], "share": share}
        for name, share in named.items()
    ]
    return _weighted(rows)


def _memory_rows(shares: Mapping[str, float]) -> list[dict[str, Any]]:
    by_memory: defaultdict[int, float] = defaultdict(float)
    for name, share in shares.items():
        if name == "Other":
            continue
        number = name.removesuffix("GB")
        if not number.isdigit() or int(number) < 1:
            raise MachineTableError(f"{name!r} is no size of memory in whole GB")
        by_memory[approximated_device_memory(int(number) * _MIB_PER_GIB)] += share
    return _weighted([{"memory": memory, "share": share} for memory, share in sorted(by_memory.items())])


def _display_rows(statistics: Statistics) -> list[dict[str, Any]]:
    shares = display_shares(statistics.css_displays.shares, statistics.pixel_displays.shares)
    rows: list[dict[str, Any]] = []
    for _, windows in _WINDOWS.values():
        for (display, share), (window, window_share) in itertools.product(shares.items(), _WINDOW_SHARES.items()):
            viewport_width, viewport_height = _VIEWPORTS[window](display, windows)
            rows.append(
                {
                    "windows": windows,
                    "window": window,
                    "display_width": display.width,
                    "display_height": display.height,
                    "scale": _json_number(display.scale),
                    "viewport_width": viewport_width,
                    "viewport_height": viewport_height,
                    "share": share * window_share,
                }
            )
    # Rows for each Windows are drawn apart from the other's, so each sums to the whole.
    return [row for windows in (11, 10) for row in _weighted([row for row in rows if row["windows"] == windows])]


# The window, in device-independent pixels, as read on 2026-11-08: the taskbar, which Windows 10 was read to have at 100%
# only, and Chrome's bars of a maximised window and its bars, top border and side borders of a window that is not.
_TASKBAR_HEIGHTS = {11: 48, 10: 40}
_MAXIMISED_BARS = {Fraction(1): 87, Fraction(5, 4): 86, Fraction(3, 2): 87, Fraction(2): 87}
_WINDOW_TOP = {Fraction(1): 95, Fraction(5, 4): 95, Fraction(3, 2): 94, Fraction(2): 93}
_WINDOW_SIDES = {Fraction(1): 16, Fraction(5, 4): 14, Fraction(3, 2): 15, Fraction(2): 13}

# Chrome's first window on a new profile, in GetDefaultWindowBounds() of chrome/browser/ui/window_sizer/window_sizer.cc,
# with the constants of window_sizer_aura.cc.
_WINDOW_TILE = 10
_WINDOW_MAX_DEFAULT_WIDTH = 1050
_MIN_SCREEN_WIDTH_FOR_HALVING = 1600

_MAXIMISED = "maximised"
_FIRST = "first"
_OWNER_WINDOWS = f"the owner's weights, 70% {_MAXIMISED} and 30% Chrome's {_FIRST} window on a new profile"
_WINDOW_SHARES = {_MAXIMISED: 0.7, _FIRST: 0.3}


def _work_area(display: Display, windows: int) -> tuple[int, int]:
    """Return the size of the display less the taskbar, in device-independent pixels."""
    width = display.width / display.scale
    height = display.height / display.scale - _TASKBAR_HEIGHTS[windows]
    if width.denominator != 1 or height.denominator != 1:
        raise MachineTableError(f"{display} has no whole number of device-independent pixels")
    return int(width), int(height)


def _maximised_viewport(display: Display, windows: int) -> tuple[int, int]:
    """Return the viewport of a window that fills the work area, which has no border."""
    width, height = _work_area(display, windows)
    return width, height - _MAXIMISED_BARS[display.scale]


def _first_window_viewport(display: Display, windows: int) -> tuple[int, int]:
    """Return the viewport of Chrome's first window on a new profile, which has no saved size."""
    width, height = _work_area(display, windows)
    window_width = min(width - 2 * _WINDOW_TILE, _WINDOW_MAX_DEFAULT_WIDTH)
    window_height = height - 2 * _WINDOW_TILE
    # A display of 16:10 or wider, whose work area is wide enough, takes half of it, so that two windows fit side by side.
    if display.width / display.height * 10 >= 16 and width > _MIN_SCREEN_WIDTH_FOR_HALVING:
        window_width = int(width / 2 - 1.5 * _WINDOW_TILE)
    # A portrait display takes 4:3.
    if display.height > display.width and window_height > window_width:
        window_height = (window_width // 4) * 3
    return window_width - _WINDOW_SIDES[display.scale], window_height - _WINDOW_TOP[display.scale]


_VIEWPORTS = {_MAXIMISED: _maximised_viewport, _FIRST: _first_window_viewport}


# The estimator's starting values in net/nqe/network_quality_estimator_params.cc, which a new Chrome sends.
_WIFI = {"round_trip_ms": 116, "throughput_kbps": 2658}
_ETHERNET = {"round_trip_ms": 90, "throughput_kbps": 1456}
_NETWORKS = ((_WIFI, 60), (_ETHERNET, 40))
"""The owner's weights, in percent."""


def _network_rows() -> list[dict[str, Any]]:
    return _weighted([{**network, "share": share} for network, share in _NETWORKS])


def _rows(name: str, shares: Mapping[str, float]) -> list[dict[str, Any]]:
    return _weighted([{name: value, "share": share} for value, share in shares.items()])


def _weighted(rows: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Return rows with their shares as whole weights in _PARTS, the largest first, and without those that round to none."""
    listed = list(rows)
    total = sum(row["share"] for row in listed)
    if total <= 0:
        raise MachineTableError("a group of the table has no share at all")
    weighted = [
        {
            **{name: value for name, value in row.items() if name != "share"},
            "weight": round(row["share"] / total * _PARTS),
        }
        for row in listed
    ]
    return sorted((row for row in weighted if row["weight"] > 0), key=lambda row: -row["weight"])


def _sizes(shares: Mapping[str, float]) -> dict[tuple[int, int], float]:
    sizes: dict[tuple[int, int], float] = {}
    for name, share in shares.items():
        width, separator, height = name.partition("x")
        if separator and width.isdigit() and height.isdigit():
            sizes[(int(width), int(height))] = share
    return sizes


def _json_number(number: Fraction) -> int | float:
    return int(number) if number.denominator == 1 else float(number)
