"""The Machine a Session claims to run on: what Chrome reads of the computer, and the client hints it makes of that.

A Machine is drawn from a table of weighted rows that ships with mitto, in machine_table.json, which tools/release writes
from public statistics. See docs/persona.md, "A machine of its own", for what decides each value and where the weights come
from. The table is under CC BY-SA 3.0, and the code here under the licence of the rest of mitto.
"""

import functools
import hashlib
import json
import math
import secrets
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any, Literal, Self, TypedDict, cast

from mitto._arguments import require_type

__all__ = [
    "MACHINE_HINTS",
    "ColorScheme",
    "Machine",
    "NoiseSalts",
    "ReducedMotion",
    "ReducedTransparency",
    "client_hints",
    "draw_noise_salts",
    "effective_connection_type",
    "rounded_downlink",
    "rounded_rtt",
    "windows_of",
]

type ColorScheme = Literal["light", "dark"]
type ReducedMotion = Literal["no-preference", "reduce"]
type ReducedTransparency = Literal["no-preference", "reduce"]

MACHINE_HINTS = frozenset(
    {
        "device-memory",
        "sec-ch-device-memory",
        "dpr",
        "sec-ch-dpr",
        "viewport-width",
        "sec-ch-viewport-width",
        "sec-ch-viewport-height",
        "rtt",
        "downlink",
        "ect",
        "sec-ch-prefers-color-scheme",
        "sec-ch-prefers-reduced-motion",
        "sec-ch-prefers-reduced-transparency",
        "sec-ch-ua-platform-version",
    }
)
"""The client hints whose values a Session works out from its Machine, by lower-case name."""

_TABLE_PATH = Path(__file__).with_name("machine_table.json")

# What Chrome on a desktop sends for the memory, which it takes to the nearer power of two and holds between 2 and 32 GiB.
# Upstream: third_party/blink/common/device_memory/approximated_device_memory.cc
_MEMORY_SIZES = (2, 4, 8, 16, 32)

# The scales that Windows' display settings offer.
_SCALES = tuple(Fraction(percent, 100) for percent in (100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500))

# Each value of sec-ch-ua-platform-version that a Windows 10 or 11 can give, and which of the two it is. The version is the
# one of Windows.Foundation.UniversalApiContract, which Chrome reads from the registry.
_PLATFORM_VERSIONS = {
    "1.0.0": 10,
    "2.0.0": 10,
    "3.0.0": 10,
    "4.0.0": 10,
    "5.0.0": 10,
    "6.0.0": 10,
    "7.0.0": 10,
    "8.0.0": 10,
    "10.0.0": 10,
    "14.0.0": 11,
    "15.0.0": 11,
    "19.0.0": 11,
}

_COLOR_SCHEMES = ("light", "dark")
_REDUCED = ("no-preference", "reduce")

# Chrome holds the throughput as 32 bits of kbps, and nothing larger is a round trip either.
_LARGEST_ESTIMATE = 2**31 - 1

# How Chrome caps and rounds the estimates before it sends them, ported from content/browser/client_hints/client_hints.cc
# and from Blink's network_state_notifier.cc.
# Upstream: content/browser/client_hints/client_hints.cc
# Upstream: third_party/blink/renderer/platform/network/network_state_notifier.cc
_LARGEST_RTT_MS = 3000
_RTT_GRANULARITY_MS = 50
_LARGEST_DOWNLINK_KBPS = 10_000
_DOWNLINK_GRANULARITY_KBPS = 50

# The round trip from which Chrome's network quality estimator gives each effective connection type, slowest first, in
# net/nqe/network_quality_estimator_params.cc.
# Upstream: net/nqe/network_quality_estimator_params.cc
_ECT_THRESHOLDS_MS = (("slow-2g", 2010), ("2g", 1420), ("3g", 272))
_FASTEST_ECT = "4g"

# The groups of the table, in the order they are drawn, and the fields each sets. A group's rows hold those fields. The
# rows of the platform version and of the display also hold which Windows they are for, because its taskbar changes the
# viewport, and a row of the display says which window it has. Neither is a field of a Machine.
_GROUPS: dict[str, tuple[str, ...]] = {
    "platform_version": ("platform_version",),
    "display": ("display_width", "display_height", "scale", "viewport_width", "viewport_height"),
    "memory": ("memory",),
    "network": ("round_trip_ms", "throughput_kbps"),
    "color_scheme": ("color_scheme",),
    "reduced_motion": ("reduced_motion",),
    "reduced_transparency": ("reduced_transparency",),
}
_WINDOWS = "windows"
_WEIGHT = "weight"

# The seed of a Machine that a caller gives none for: as many bits as a seed needs to be its own.
_SEED_BITS = 64

# The browser process of Chrome draws its salt for the noise on rtt and downlink from 1 to 21, in
# content/browser/client_hints/client_hints.cc. The native core works out the salt of each renderer process from a key.
_BROWSER_SALTS = 21
_RENDERER_SALT_KEY_BYTES = 32

_MITTO_0_1 = "mitto-0.1"


@dataclass(frozen=True, slots=True, kw_only=True)
class Machine:
    """The computer a Session claims to run on, as Chrome reads it. A Session works out each client hint from it.

    It is apart from the Host and from the Persona, and never read from the computer mitto runs on. draw() gives one from
    the table that ships with mitto.

    Attributes:
        memory: The memory in GiB, as Chrome rounds what Windows reports: 2, 4, 8, 16 or 32.
        display_width: The width of the primary display, in pixels.
        display_height: The height of the primary display, in pixels.
        scale: The scale in Windows' display settings, such as 1.25 for 125%.
        viewport_width: The width of the window's page area, in device-independent pixels.
        viewport_height: The height of the window's page area, in device-independent pixels.
        round_trip_ms: The round trip of HTTP that Chrome's network quality estimator has, in milliseconds.
        throughput_kbps: The throughput that the estimator has, in kbps.
        color_scheme: "light" or "dark", as Windows' app mode and Chrome's theme leave it.
        reduced_motion: "no-preference", or "reduce" with Windows' animation effects off.
        reduced_transparency: "no-preference", or "reduce" with Windows' transparency effects off.
        platform_version: The version of Windows' UniversalApiContract, as sec-ch-ua-platform-version writes it, such as
            "19.0.0" for Windows 11 24H2.
        seed: What draw() drew it from, or None for a Machine that a caller made.

    Raises:
        TypeError: If a value is of a type it does not take, which the message names. True and False are no number.
        ValueError: If a value is not one that Chrome on Windows 10 or 11 can read, or the viewport is larger than the
            display.
    """

    memory: int
    display_width: int
    display_height: int
    scale: float
    viewport_width: int
    viewport_height: int
    round_trip_ms: int
    throughput_kbps: int
    color_scheme: ColorScheme
    reduced_motion: ReducedMotion
    reduced_transparency: ReducedTransparency
    platform_version: str
    seed: int | None = None

    def __post_init__(self) -> None:
        for name, check in _CHECKS.items():
            check(name, getattr(self, name))
        if self.seed is not None:
            _require_whole_number("seed", self.seed)
        _check_viewport(self)

    @classmethod
    def draw(
        cls,
        *,
        seed: int | None = None,
        memory: int | None = None,
        display_width: int | None = None,
        display_height: int | None = None,
        scale: float | None = None,
        viewport_width: int | None = None,
        viewport_height: int | None = None,
        round_trip_ms: int | None = None,
        throughput_kbps: int | None = None,
        color_scheme: ColorScheme | None = None,
        reduced_motion: ReducedMotion | None = None,
        reduced_transparency: ReducedTransparency | None = None,
        platform_version: str | None = None,
    ) -> Self:
        """Return a Machine drawn from the table that ships with mitto, with the values given here and the rest to fit them.

        The display, its scale and the viewport are drawn together, and so are the round trip and the throughput. Of such
        a group, give all its values, or only values that a row of the table has. The other values are drawn on their own.
        The same seed and table give the same Machine, on every Host.

        Args:
            seed: What to draw from. None draws a seed, which the Machine keeps.
            memory: As in Machine, or None to draw it. So for each of the others.
            display_width: See memory.
            display_height: See memory.
            scale: See memory.
            viewport_width: See memory.
            viewport_height: See memory.
            round_trip_ms: See memory.
            throughput_kbps: See memory.
            color_scheme: See memory.
            reduced_motion: See memory.
            reduced_transparency: See memory.
            platform_version: See memory.

        Raises:
            TypeError: As Machine does, and if `seed` is not a whole number or None.
            ValueError: As Machine does, and if no row of the table fits the values given of a group whose values are not
                all given.
        """
        if seed is not None:
            _require_whole_number("seed", seed)
        given: dict[str, object] = {
            "memory": memory,
            "display_width": display_width,
            "display_height": display_height,
            "scale": scale,
            "viewport_width": viewport_width,
            "viewport_height": viewport_height,
            "round_trip_ms": round_trip_ms,
            "throughput_kbps": throughput_kbps,
            "color_scheme": color_scheme,
            "reduced_motion": reduced_motion,
            "reduced_transparency": reduced_transparency,
            "platform_version": platform_version,
        }
        fixed = {name: value for name, value in given.items() if value is not None}
        for name, value in fixed.items():
            _CHECKS[name](name, value)
        chosen_seed = secrets.randbits(_SEED_BITS) if seed is None else seed
        values: dict[str, Any] = draw_values(load_table(), chosen_seed, fixed)
        return cls(**values, seed=chosen_seed)

    @classmethod
    def named(cls, name: str) -> Self:
        """Return the Machine of this name: "mitto-0.1" is the one that every Session of mitto 0.1 claimed.

        Raises:
            TypeError: If `name` is not text.
            ValueError: If no Machine has this name.
        """
        require_type("name", name, str, "text")
        if name != _MITTO_0_1:
            raise ValueError(f"no Machine is named {name!r}, only {_MITTO_0_1!r}")
        # A 1920 by 1080 display at 100%, a window that fills it on Windows 11 24H2, and a good connection.
        return cls(
            memory=8,
            display_width=1920,
            display_height=1080,
            scale=1,
            viewport_width=1920,
            viewport_height=945,
            round_trip_ms=50,
            throughput_kbps=10_000,
            color_scheme="light",
            reduced_motion="no-preference",
            reduced_transparency="no-preference",
            platform_version="19.0.0",
        )


@dataclass(frozen=True, slots=True)
class NoiseSalts:
    """The salts of the noise that one run of Chrome adds to rtt and downlink for each host.

    Attributes:
        browser: The salt of the browser process, from 1 to 21, which the noise of a navigation takes.
        renderer_key: What the native core works out the salt of the renderer process of each site from, from 1 to 20,
            which the noise of a script, image or fetch takes.
    """

    browser: int
    renderer_key: str


def draw_noise_salts(machine: Machine) -> NoiseSalts:
    """Return the salts of a run of Chrome on `machine`: from its seed, or at random for a Machine with none.

    Chrome draws them again each time it starts, so they are not part of the Machine, and a Session draws them each time
    it opens. The same seed gives the same salts, so that the same Machine sends the same values again.
    """
    if machine.seed is None:
        return NoiseSalts(
            browser=1 + secrets.randbelow(_BROWSER_SALTS), renderer_key=secrets.token_hex(_RENDERER_SALT_KEY_BYTES)
        )
    renderer_key = _point(machine.seed, "renderer_salt_key").to_bytes(_RENDERER_SALT_KEY_BYTES).hex()
    return NoiseSalts(browser=1 + _point(machine.seed, "browser_salt") % _BROWSER_SALTS, renderer_key=renderer_key)


def client_hints(machine: Machine) -> dict[str, str]:
    """Return the value of each client hint that follows the machine, by lower-case name, as Chrome works it out.

    rtt and downlink have Chrome's cap and rounding but no noise, which the native core adds on each hop, for its host.
    """
    memory = str(machine.memory)
    dpr = _number_text(machine.scale)
    viewport_width = str(machine.viewport_width)
    return {
        "device-memory": memory,
        "sec-ch-device-memory": memory,
        "dpr": dpr,
        "sec-ch-dpr": dpr,
        "viewport-width": viewport_width,
        "sec-ch-viewport-width": viewport_width,
        "sec-ch-viewport-height": str(machine.viewport_height),
        "rtt": str(rounded_rtt(machine.round_trip_ms)),
        "downlink": _number_text(rounded_downlink(machine.throughput_kbps)),
        "ect": effective_connection_type(machine.round_trip_ms),
        "sec-ch-prefers-color-scheme": machine.color_scheme,
        "sec-ch-prefers-reduced-motion": machine.reduced_motion,
        "sec-ch-prefers-reduced-transparency": machine.reduced_transparency,
        "sec-ch-ua-platform-version": f'"{machine.platform_version}"',
    }


def rounded_rtt(round_trip_ms: int) -> int:
    """Return the rtt that Chrome sends for a round trip with no noise: at most 3000, to the nearest 50 ms, half up."""
    capped = min(round_trip_ms, _LARGEST_RTT_MS)
    return (capped + _RTT_GRANULARITY_MS // 2) // _RTT_GRANULARITY_MS * _RTT_GRANULARITY_MS


def rounded_downlink(throughput_kbps: int) -> float:
    """Return the downlink in Mbps that Chrome sends for a throughput with no noise: at most 10, to the nearest 50 kbps."""
    capped = min(throughput_kbps, _LARGEST_DOWNLINK_KBPS)
    rounded = (capped + _DOWNLINK_GRANULARITY_KBPS // 2) // _DOWNLINK_GRANULARITY_KBPS * _DOWNLINK_GRANULARITY_KBPS
    return rounded / 1000


def effective_connection_type(round_trip_ms: int) -> str:
    """Return the ect that Chrome's network quality estimator gives a round trip."""
    return next((name for name, threshold in _ECT_THRESHOLDS_MS if round_trip_ms >= threshold), _FASTEST_ECT)


def windows_of(platform_version: str) -> int:
    """Return 10 or 11, the Windows that gives this sec-ch-ua-platform-version.

    Raises:
        ValueError: If neither gives it.
    """
    windows = _PLATFORM_VERSIONS.get(platform_version)
    if windows is None:
        known = ", ".join(repr(version) for version in _PLATFORM_VERSIONS)
        raise ValueError(
            f"platform_version has to be one that Windows 10 or 11 gives, {known}, not {platform_version!r}"
        )
    return windows


def _number_text(number: float) -> str:
    """Return a number as Chrome writes it in a hint: the shortest decimal, with no point for a whole number."""
    return str(int(number)) if float(number).is_integer() else repr(float(number))


# Checks of each value, which draw() runs on what a caller gives before it draws the rest.


def _require_whole_number(name: str, value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} has to be a whole number, not {type(value).__name__}")


def _require_text(name: str, value: object) -> None:
    if not isinstance(value, str):
        raise TypeError(f"{name} has to be text, not {type(value).__name__}")


def _check_memory(name: str, value: object) -> None:
    _require_whole_number(name, value)
    if value not in _MEMORY_SIZES:
        raise ValueError(f"{name} has to be 2, 4, 8, 16 or 32, as Chrome rounds it, not {value}")


def _check_size(name: str, value: object) -> None:
    _require_whole_number(name, value)
    if cast(int, value) < 1:
        raise ValueError(f"{name} has to be at least 1, not {value}")


def _check_scale(name: str, value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        raise TypeError(f"{name} has to be a number, not {type(value).__name__}")
    if not (math.isfinite(value) and Fraction(value) in _SCALES):
        allowed = ", ".join(_number_text(float(scale)) for scale in _SCALES)
        raise ValueError(f"{name} has to be one that Windows offers, {allowed}, not {value}")


def _check_estimate(name: str, value: object) -> None:
    _require_whole_number(name, value)
    if not 0 <= cast(int, value) <= _LARGEST_ESTIMATE:
        raise ValueError(f"{name} has to be from 0 to {_LARGEST_ESTIMATE}, not {value}")


def _check_one_of(allowed: Sequence[str]) -> Callable[[str, object], None]:
    def check(name: str, value: object) -> None:
        _require_text(name, value)
        if value not in allowed:
            raise ValueError(f"{name} has to be {' or '.join(repr(text) for text in allowed)}, not {value!r}")

    return check


def _check_platform_version(name: str, value: object) -> None:
    _require_text(name, value)
    windows_of(cast(str, value))


_CHECKS: dict[str, Callable[[str, object], None]] = {
    "memory": _check_memory,
    "display_width": _check_size,
    "display_height": _check_size,
    "scale": _check_scale,
    "viewport_width": _check_size,
    "viewport_height": _check_size,
    "round_trip_ms": _check_estimate,
    "throughput_kbps": _check_estimate,
    "color_scheme": _check_one_of(_COLOR_SCHEMES),
    "reduced_motion": _check_one_of(_REDUCED),
    "reduced_transparency": _check_one_of(_REDUCED),
    "platform_version": _check_platform_version,
}


def _check_viewport(machine: Machine) -> None:
    """Refuse a viewport larger than the display. A window can fill the display and no more, as Chrome in full screen does."""
    scale = Fraction(machine.scale)
    widest = math.ceil(machine.display_width / scale)
    highest = math.ceil(machine.display_height / scale)
    if machine.viewport_width > widest or machine.viewport_height > highest:
        raise ValueError(
            f"the viewport of {machine.viewport_width} by {machine.viewport_height} does not fit on a display of"
            f" {widest} by {highest} device-independent pixels"
        )


# The table


class TableGroup(TypedDict):
    """One group of the table: rows of the values drawn together, each with its weight, and where the weights come from."""

    source: str
    rows: list[dict[str, Any]]


class Table(TypedDict):
    """What machine_table.json holds."""

    licence: str
    credits: list[str]
    groups: dict[str, TableGroup]


@functools.cache
def load_table() -> Table:
    """Return the table that ships with mitto."""
    return cast(Table, json.loads(_TABLE_PATH.read_text(encoding="utf-8")))


def draw_values(table: Table, seed: int, fixed: Mapping[str, object]) -> dict[str, object]:
    """Return a value for each field of a Machine but its seed: those in `fixed`, and the rest drawn from `table` to fit.

    Each group is drawn from a number of its own that comes from the seed, so a value fixed in one group leaves the draw of
    another as it was, unless the two have to fit together.

    Raises:
        ValueError: If no row of a group fits what is fixed of it, and not all of it is fixed.
    """
    values = dict(fixed)
    if "platform_version" in values:
        values[_WINDOWS] = windows_of(cast(str, values["platform_version"]))
    to_draw = [(group, names) for group, names in _GROUPS.items() if not all(name in values for name in names)]
    for group, names in to_draw:
        if not _any_fits(table["groups"][group]["rows"], values):
            given = [f"{name}={values[name]!r}" for name in names if name in values]
            if _WINDOWS in values and group == "display":
                given.append(f"on Windows {values[_WINDOWS]}")
            raise ValueError(
                f"no row of mitto's table has {', '.join(given)}: give all of {', '.join(names)}, or only values that a"
                " row has"
            )
    for number, (group, _) in enumerate(to_draw):
        later = [table["groups"][later_group]["rows"] for later_group, _ in to_draw[number + 1 :]]
        # A row that leaves no row of a later group to fit could not be drawn with what is fixed there.
        rows = [
            row
            for row in table["groups"][group]["rows"]
            if _fits(row, values) and all(_any_fits(later_rows, {**values, **row}) for later_rows in later)
        ]
        values.update(_weighted_choice(rows, _point(seed, group)))
    return {name: values[name] for names in _GROUPS.values() for name in names}


def _fits(row: Mapping[str, object], values: Mapping[str, object]) -> bool:
    """Return whether a row of the table agrees with each value it has that is set already."""
    return all(name == _WEIGHT or name not in values or values[name] == value for name, value in row.items())


def _any_fits(rows: Sequence[Mapping[str, object]], values: Mapping[str, object]) -> bool:
    return any(_fits(row, values) for row in rows)


def _point(seed: int, group: str) -> int:
    """Return a whole number for drawing a group from a seed, which is the same on every Host and in every Python."""
    return int.from_bytes(hashlib.sha256(f"mitto machine {seed} {group}".encode()).digest())


def _weighted_choice(rows: Sequence[Mapping[str, object]], point: int) -> dict[str, object]:
    """Return the values of the row that `point` lands on, where each row takes as many of the points as its weight."""
    weights = [cast(int, row[_WEIGHT]) for row in rows]
    landing = point % sum(weights)
    for row, weight in zip(rows, weights, strict=True):
        if landing < weight:
            return {name: value for name, value in row.items() if name != _WEIGHT}
        landing -= weight
    raise AssertionError("the point lands past the last row")
