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
from dataclasses import dataclass, fields
from fractions import Fraction
from pathlib import Path
from typing import Any, Literal, Self, TypedDict, cast

from mitto._arguments import require_type
from mitto._window import Rect, maximised_viewport, normal_window_around, work_area

__all__ = [
    "MACHINE_HINTS",
    "ColorScheme",
    "ContrastTheme",
    "InvertedColors",
    "Machine",
    "NoiseSalts",
    "ReducedMotion",
    "ReducedTransparency",
    "WindowState",
    "client_hints",
    "draw_noise_salts",
    "effective_connection_type",
    "machine_from_saved",
    "rounded_downlink",
    "rounded_rtt",
    "windows_of",
    "windows_time_zones",
]

type ColorScheme = Literal["light", "dark"]
type ReducedMotion = Literal["no-preference", "reduce"]
type ReducedTransparency = Literal["no-preference", "reduce"]
type WindowState = Literal["maximised", "normal"]
type ContrastTheme = Literal[
    "none",
    "aquatic",
    "desert",
    "dusk",
    "night-sky",
    "high-contrast-1",
    "high-contrast-2",
    "high-contrast-black",
    "high-contrast-white",
]
type InvertedColors = Literal["none", "inverted"]

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
_TIME_ZONES_PATH = Path(__file__).with_name("time_zones.json")

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
_WINDOW_STATES = ("maximised", "normal")
_INVERTED_COLORS = ("none", "inverted")
_NO_CONTRAST_THEME = "none"

# Windows' contrast themes: which Windows offers each, and the colour scheme that Chrome 157 takes from it, by the
# lightness of its Window colour, as docs/persona.md, "Contrast themes and colour filters", read them. Chrome turns on forced
# colours with any of them, and finds prefers-contrast: more for a contrast of at least 7:1 between its Window and
# WindowText colours, which Windows 11's have.
# Upstream: ui/native_theme/os_settings_provider.cc
_CONTRAST_THEMES: Mapping[str, tuple[int, ColorScheme]] = {
    "aquatic": (11, "dark"),
    "desert": (11, "light"),
    "dusk": (11, "dark"),
    "night-sky": (11, "dark"),
    "high-contrast-1": (10, "dark"),
    "high-contrast-2": (10, "dark"),
    "high-contrast-black": (10, "dark"),
    "high-contrast-white": (10, "light"),
}

# navigator.hardwareConcurrency is the number of logical processors in Chrome's processor group, which Windows holds to 64.
# Upstream: base/system/sys_info_win.cc
_MOST_PROCESSORS = 64

# navigator.maxTouchPoints is what Windows says a touch screen takes, and 0 with none.
# Upstream: ui/base/pointer/pointer_device_win.cc
_MOST_TOUCH_POINTS = 256

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

_WORK_AREA = ("work_area_x", "work_area_y", "work_area_width", "work_area_height")
_WINDOW = ("window_x", "window_y", "window_width", "window_height")

# The groups of the table, in the order they are drawn, and the fields each sets. A group's rows hold those fields. The
# rows of the platform version and of the display also hold which Windows they are for, because its taskbar changes the
# work area and the viewport, and a row of the time zone the locale it is for. Neither is a field of a Machine.
_GROUPS: dict[str, tuple[str, ...]] = {
    "platform_version": ("platform_version",),
    "display": (
        "display_width",
        "display_height",
        "scale",
        *_WORK_AREA,
        "window_state",
        *_WINDOW,
        "viewport_width",
        "viewport_height",
    ),
    "memory": ("memory",),
    "network": ("round_trip_ms", "throughput_kbps"),
    "color_scheme": ("color_scheme",),
    "reduced_motion": ("reduced_motion",),
    "reduced_transparency": ("reduced_transparency",),
    "contrast_theme": ("contrast_theme",),
    "inverted_colors": ("inverted_colors",),
    "time_zone": ("time_zone",),
    "processors": ("processors",),
    "touch_points": ("touch_points",),
}
_WINDOWS = "windows"
_WEIGHT = "weight"
_DISPLAY = "display"

# What a display that the table does not have needs, for its work area and window to be worked out: the fields that a
# Machine of mitto 0.2 held of it.
_DISPLAY_OF_0_2 = ("display_width", "display_height", "scale", "viewport_width", "viewport_height")

# The fields that mitto 0.2 did not know. A Machine that it saved lacks them.
_ADDED_AFTER_0_2 = frozenset(
    {
        *_WORK_AREA,
        "window_state",
        *_WINDOW,
        "contrast_theme",
        "inverted_colors",
        "time_zone",
        "processors",
        "touch_points",
    }
)

# The seed of a Machine that a caller gives none for: as many bits as a seed needs to be its own.
_SEED_BITS = 64

# The browser process of Chrome draws its salt for the noise on rtt and downlink from 1 to 21, in
# content/browser/client_hints/client_hints.cc. The native core works out the salt of each renderer process from a key.
_BROWSER_SALTS = 21
_RENDERER_SALT_KEY_BYTES = 32

_MITTO_0_1 = "mitto-0.1"


@dataclass(frozen=True, slots=True, kw_only=True)
class Machine:
    """The computer a Session claims to run on, as Chrome reads it. A Session works out each client hint from it, and a
    Page what its scripts read of the computer.

    It is apart from the Host and from the Persona, and never read from the computer mitto runs on. draw() gives one from
    the table that ships with mitto. Sizes and places on the desktop are in device-independent pixels, which are the
    display's pixels over the scale, from the display's top left corner.

    Attributes:
        memory: The memory in GiB, as Chrome rounds what Windows reports: 2, 4, 8, 16 or 32.
        display_width: The width of the primary display, in pixels.
        display_height: The height of the primary display, in pixels.
        scale: The scale in Windows' display settings, such as 1.25 for 125%.
        work_area_x: Where the work area starts from the left, which is the display less the taskbar.
        work_area_y: Where the work area starts from the top.
        work_area_width: The width of the work area.
        work_area_height: The height of the work area.
        window_state: "maximised", for a window that fills the work area, or "normal".
        window_x: Where Chrome's window starts from the left.
        window_y: Where Chrome's window starts from the top.
        window_width: The width of Chrome's window, with its bars and borders.
        window_height: The height of Chrome's window, with its bars and borders.
        viewport_width: The width of the window's page area.
        viewport_height: The height of the window's page area.
        round_trip_ms: The round trip of HTTP that Chrome's network quality estimator has, in milliseconds.
        throughput_kbps: The throughput that the estimator has, in kbps.
        color_scheme: "light" or "dark", as Windows' app mode and Chrome's theme leave it.
        reduced_motion: "no-preference", or "reduce" with Windows' animation effects off.
        reduced_transparency: "no-preference", or "reduce" with Windows' transparency effects off.
        contrast_theme: Windows' contrast theme, such as "aquatic", or "none".
        inverted_colors: "none", or "inverted" with Windows' colour filter that inverts colours.
        platform_version: The version of Windows' UniversalApiContract, as sec-ch-ua-platform-version writes it, such as
            "19.0.0" for Windows 11 24H2.
        time_zone: The time zone, by the name that Chrome on Windows gives it, such as "America/New_York".
        processors: The number of logical processors.
        touch_points: How many touch points the touch screen takes, or 0 for none.
        seed: What draw() drew it from, or None for a Machine that a caller made.

    Raises:
        TypeError: If a value is of a type it does not take, which the message names. True and False are no number.
        ValueError: If a value is not one that Chrome on Windows 10 or 11 can read, or values do not go together, such as
            a viewport larger than the display.
    """

    memory: int
    display_width: int
    display_height: int
    scale: float
    work_area_x: int
    work_area_y: int
    work_area_width: int
    work_area_height: int
    window_state: WindowState
    window_x: int
    window_y: int
    window_width: int
    window_height: int
    viewport_width: int
    viewport_height: int
    round_trip_ms: int
    throughput_kbps: int
    color_scheme: ColorScheme
    reduced_motion: ReducedMotion
    reduced_transparency: ReducedTransparency
    contrast_theme: ContrastTheme
    inverted_colors: InvertedColors
    platform_version: str
    time_zone: str
    processors: int
    touch_points: int
    seed: int | None = None

    def __post_init__(self) -> None:
        for name, check in _CHECKS.items():
            check(name, getattr(self, name))
        if self.seed is not None:
            _require_whole_number("seed", self.seed)
        _check_desktop(self)
        _check_contrast_theme(self.contrast_theme, windows_of(self.platform_version), self.color_scheme)

    @classmethod
    def draw(
        cls,
        *,
        seed: int | None = None,
        memory: int | None = None,
        display_width: int | None = None,
        display_height: int | None = None,
        scale: float | None = None,
        work_area_x: int | None = None,
        work_area_y: int | None = None,
        work_area_width: int | None = None,
        work_area_height: int | None = None,
        window_state: WindowState | None = None,
        window_x: int | None = None,
        window_y: int | None = None,
        window_width: int | None = None,
        window_height: int | None = None,
        viewport_width: int | None = None,
        viewport_height: int | None = None,
        round_trip_ms: int | None = None,
        throughput_kbps: int | None = None,
        color_scheme: ColorScheme | None = None,
        reduced_motion: ReducedMotion | None = None,
        reduced_transparency: ReducedTransparency | None = None,
        contrast_theme: ContrastTheme | None = None,
        inverted_colors: InvertedColors | None = None,
        platform_version: str | None = None,
        time_zone: str | None = None,
        processors: int | None = None,
        touch_points: int | None = None,
    ) -> Self:
        """Return a Machine drawn from the table that ships with mitto, with the values given here and the rest to fit them.

        The display, its scale, the work area, the window and the viewport are drawn together, and so are the round trip
        and the throughput. Of such a group, give all its values, or only values that a row of the table has. A display
        that the table does not have can still be given with its scale and viewport alone, and then gets the work area of
        a taskbar at its bottom and a window around the viewport. A contrast theme sets the Windows and the colour scheme
        that go with it. The other values are drawn on their own. The same seed and table give the same Machine, on every
        Host.

        Args:
            seed: What to draw from. None draws a seed, which the Machine keeps.
            memory: As in Machine, or None to draw it. So for each of the others.
            display_width: See memory.
            display_height: See memory.
            scale: See memory.
            work_area_x: See memory.
            work_area_y: See memory.
            work_area_width: See memory.
            work_area_height: See memory.
            window_state: See memory.
            window_x: See memory.
            window_y: See memory.
            window_width: See memory.
            window_height: See memory.
            viewport_width: See memory.
            viewport_height: See memory.
            round_trip_ms: See memory.
            throughput_kbps: See memory.
            color_scheme: See memory.
            reduced_motion: See memory.
            reduced_transparency: See memory.
            contrast_theme: See memory.
            inverted_colors: See memory.
            platform_version: See memory.
            time_zone: See memory.
            processors: See memory.
            touch_points: See memory.

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
            "work_area_x": work_area_x,
            "work_area_y": work_area_y,
            "work_area_width": work_area_width,
            "work_area_height": work_area_height,
            "window_state": window_state,
            "window_x": window_x,
            "window_y": window_y,
            "window_width": window_width,
            "window_height": window_height,
            "viewport_width": viewport_width,
            "viewport_height": viewport_height,
            "round_trip_ms": round_trip_ms,
            "throughput_kbps": throughput_kbps,
            "color_scheme": color_scheme,
            "reduced_motion": reduced_motion,
            "reduced_transparency": reduced_transparency,
            "contrast_theme": contrast_theme,
            "inverted_colors": inverted_colors,
            "platform_version": platform_version,
            "time_zone": time_zone,
            "processors": processors,
            "touch_points": touch_points,
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
        # A 1920 by 1080 display at 100%, a window that fills it on Windows 11 24H2, and a good connection. mitto 0.1 had
        # no Runtime, so what only a page reads is what the table draws most often.
        return cls(
            memory=8,
            display_width=1920,
            display_height=1080,
            scale=1,
            work_area_x=0,
            work_area_y=0,
            work_area_width=1920,
            work_area_height=1032,
            window_state="maximised",
            window_x=0,
            window_y=0,
            window_width=1920,
            window_height=1032,
            viewport_width=1920,
            viewport_height=945,
            round_trip_ms=50,
            throughput_kbps=10_000,
            color_scheme="light",
            reduced_motion="no-preference",
            reduced_transparency="no-preference",
            contrast_theme="none",
            inverted_colors="none",
            platform_version="19.0.0",
            time_zone="America/New_York",
            processors=8,
            touch_points=0,
        )


_FIELDS = tuple(field.name for field in fields(Machine) if field.name != "seed")


def machine_from_saved(saved: Mapping[str, object]) -> Machine:
    """Return the Machine that a file of cookies holds, as dataclasses.asdict() wrote it.

    A Machine that mitto 0.2 saved lacks the fields that it did not know. They are drawn from its seed to fit what it has,
    or for a Machine with no seed from what it has, so that it is the same Machine in every process and stays the same
    once it is saved again.

    Raises:
        TypeError: As Machine does.
        ValueError: If `saved` lacks a field that mitto 0.2 saved, or holds one that a Machine does not have, or as
            Machine and draw() do.
    """
    known = {*_FIELDS, "seed"}
    if not known - _ADDED_AFTER_0_2 <= saved.keys() <= known:
        raise ValueError(f"a saved Machine holds {', '.join(sorted(known))}, and nothing else")
    seed = saved.get("seed")
    values = {name: value for name, value in saved.items() if name != "seed"}
    if values.keys() != {*_FIELDS}:
        if seed is not None:
            _require_whole_number("seed", seed)
        for name, value in values.items():
            _CHECKS[name](name, value)
        seed_to_draw = cast(int, seed) if seed is not None else _seed_of(values)
        values = draw_values(load_table(), seed_to_draw, values)
    return Machine(**cast(dict[str, Any], values), seed=cast(int | None, seed))


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


class TimeZones(TypedDict):
    """What time_zones.json holds: the time zones that Chrome on Windows can give, and where the list comes from."""

    source: str
    licence: str
    time_zones: list[str]


@functools.cache
def windows_time_zones() -> frozenset[str]:
    """Return the name of each time zone that Chrome on Windows can give a page, from the list that ships with mitto.

    Chrome takes the zone that ICU's table of Windows' time zones gives for the zone and region in Windows' settings, and
    a page reads its canonical name.
    """
    listed = cast(TimeZones, json.loads(_TIME_ZONES_PATH.read_text(encoding="utf-8")))
    return frozenset(listed["time_zones"])


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


def _check_whole_number_from(least: int, most: int | None = None) -> Callable[[str, object], None]:
    def check(name: str, value: object) -> None:
        _require_whole_number(name, value)
        number = cast(int, value)
        if number < least or (most is not None and number > most):
            within = f"at least {least}" if most is None else f"from {least} to {most}"
            raise ValueError(f"{name} has to be {within}, not {value}")

    return check


def _check_place(name: str, value: object) -> None:
    _require_whole_number(name, value)


def _check_scale(name: str, value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        raise TypeError(f"{name} has to be a number, not {type(value).__name__}")
    if not (math.isfinite(value) and Fraction(value) in _SCALES):
        allowed = ", ".join(_number_text(float(scale)) for scale in _SCALES)
        raise ValueError(f"{name} has to be one that Windows offers, {allowed}, not {value}")


def _check_one_of(allowed: Sequence[str]) -> Callable[[str, object], None]:
    def check(name: str, value: object) -> None:
        _require_text(name, value)
        if value not in allowed:
            raise ValueError(f"{name} has to be {' or '.join(repr(text) for text in allowed)}, not {value!r}")

    return check


def _check_platform_version(name: str, value: object) -> None:
    _require_text(name, value)
    windows_of(cast(str, value))


def _check_time_zone(name: str, value: object) -> None:
    _require_text(name, value)
    if value not in windows_time_zones():
        raise ValueError(
            f"{name} has to be a time zone that Chrome on Windows gives, by its canonical name, such as"
            f" 'America/New_York' or 'Asia/Calcutta', not {value!r}"
        )


_CHECKS: dict[str, Callable[[str, object], None]] = {
    "memory": _check_memory,
    "display_width": _check_whole_number_from(1),
    "display_height": _check_whole_number_from(1),
    "scale": _check_scale,
    "work_area_x": _check_whole_number_from(0),
    "work_area_y": _check_whole_number_from(0),
    "work_area_width": _check_whole_number_from(1),
    "work_area_height": _check_whole_number_from(1),
    "window_state": _check_one_of(_WINDOW_STATES),
    "window_x": _check_place,
    "window_y": _check_place,
    "window_width": _check_whole_number_from(1),
    "window_height": _check_whole_number_from(1),
    "viewport_width": _check_whole_number_from(1),
    "viewport_height": _check_whole_number_from(1),
    "round_trip_ms": _check_whole_number_from(0, _LARGEST_ESTIMATE),
    "throughput_kbps": _check_whole_number_from(0, _LARGEST_ESTIMATE),
    "color_scheme": _check_one_of(_COLOR_SCHEMES),
    "reduced_motion": _check_one_of(_REDUCED),
    "reduced_transparency": _check_one_of(_REDUCED),
    "contrast_theme": _check_one_of((_NO_CONTRAST_THEME, *_CONTRAST_THEMES)),
    "inverted_colors": _check_one_of(_INVERTED_COLORS),
    "platform_version": _check_platform_version,
    "time_zone": _check_time_zone,
    "processors": _check_whole_number_from(1, _MOST_PROCESSORS),
    "touch_points": _check_whole_number_from(0, _MOST_TOUCH_POINTS),
}


def _check_desktop(machine: Machine) -> None:
    """Refuse a work area, window or viewport that do not fit together on the display.

    The work area lies on the display, a maximised window is the work area, and the page area lies inside the window. A
    viewport can be as large as the display and no larger, as Chrome in full screen has it.
    """
    scale = Fraction(machine.scale)
    widest = math.ceil(machine.display_width / scale)
    highest = math.ceil(machine.display_height / scale)
    if machine.viewport_width > widest or machine.viewport_height > highest:
        raise ValueError(
            f"the viewport of {machine.viewport_width} by {machine.viewport_height} does not fit on a display of"
            f" {widest} by {highest} device-independent pixels"
        )
    area = Rect(*(getattr(machine, name) for name in _WORK_AREA))
    if area.x + area.width > widest or area.y + area.height > highest:
        raise ValueError(f"the work area {tuple(area)} does not fit on a display of {widest} by {highest}")
    window = Rect(*(getattr(machine, name) for name in _WINDOW))
    if machine.window_state == "maximised" and window != area:
        raise ValueError(f"a maximised window is the work area {tuple(area)}, not {tuple(window)}")
    if machine.viewport_width > window.width or machine.viewport_height > window.height:
        raise ValueError(
            f"the viewport of {machine.viewport_width} by {machine.viewport_height} does not fit in a window of"
            f" {window.width} by {window.height}"
        )


def _check_contrast_theme(theme: str, windows: int, color_scheme: str) -> None:
    """Refuse a contrast theme that the Windows does not offer, or a colour scheme that Chrome 157 does not take from it."""
    if theme == _NO_CONTRAST_THEME:
        return
    theme_windows, theme_scheme = _CONTRAST_THEMES[theme]
    if windows != theme_windows:
        raise ValueError(f"the contrast theme {theme!r} is one of Windows {theme_windows}, not of Windows {windows}")
    if color_scheme != theme_scheme:
        raise ValueError(f"the contrast theme {theme!r} gives the colour scheme {theme_scheme!r}, not {color_scheme!r}")


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
    another as it was, unless the two have to fit together. A display that no row has, given with its scale and viewport,
    gets the rest of its group worked out.

    Raises:
        ValueError: If no row of a group fits what is fixed of it, and not all of it is fixed.
    """
    values = dict(fixed)
    _fix_what_follows(values)
    to_draw = [group for group, names in _GROUPS.items() if not all(name in values for name in names)]
    for group in to_draw:
        if not _group_fits(table, group, values, work_out=True):
            names = _GROUPS[group]
            given = [f"{name}={values[name]!r}" for name in names if name in values]
            if _WINDOWS in values and group == _DISPLAY:
                given.append(f"on Windows {values[_WINDOWS]}")
            raise ValueError(
                f"no row of mitto's table has {', '.join(given)}: give all of {', '.join(names)}, or only values that a"
                " row has"
            )
    for number, group in enumerate(to_draw):
        later = to_draw[number + 1 :]
        fitting = [row for row in table["groups"][group]["rows"] if _fits(row, values)]
        # A row that leaves no row of a later group to fit could not be drawn with what is fixed there. A display worked
        # out from its viewport fits only once no row of the table does.
        rows = [row for row in fitting if all(_group_fits(table, other, {**values, **row}) for other in later)] or [
            row
            for row in fitting
            if all(_group_fits(table, other, {**values, **row}, work_out=True) for other in later)
        ]
        if rows:
            values.update(_weighted_choice(rows, _point(seed, group)))
        else:
            values.update(_worked_out_window(values))
    return {name: values[name] for names in _GROUPS.values() for name in names}


def _fix_what_follows(values: dict[str, object]) -> None:
    """Add to what is fixed what follows from it: the Windows of a platform version or contrast theme, and the colour
    scheme of a contrast theme.

    Raises:
        ValueError: If the platform version and the contrast theme are of different Windows.
    """
    if "platform_version" in values:
        values[_WINDOWS] = windows_of(cast(str, values["platform_version"]))
    theme = values.get("contrast_theme")
    if theme is None or theme == _NO_CONTRAST_THEME:
        return
    windows, color_scheme = _CONTRAST_THEMES[cast(str, theme)]
    if values.setdefault(_WINDOWS, windows) != windows:
        raise ValueError(f"the contrast theme {theme!r} is one of Windows {windows}, not of Windows {values[_WINDOWS]}")
    values.setdefault("color_scheme", color_scheme)


def _group_fits(table: Table, group: str, values: Mapping[str, object], *, work_out: bool = False) -> bool:
    """Return whether a row of a group fits `values`, or, with `work_out`, whether the display can be worked out."""
    if _any_fits(table["groups"][group]["rows"], values):
        return True
    return work_out and group == _DISPLAY and all(name in values for name in _DISPLAY_OF_0_2)


def _worked_out_window(values: Mapping[str, object]) -> dict[str, object]:
    """Return the work area and the window of a display that no row has, from its scale and viewport, where not given.

    The taskbar is at the bottom, as Windows sets it up. A viewport that fills the work area less Chrome's bars is a
    maximised window's, and any other is the page area of a window that is not maximised, where Chrome puts its first.
    """
    scale = Fraction(cast(float, values["scale"]))
    if all(name in values for name in _WORK_AREA):
        area = Rect(*(cast(int, values[name]) for name in _WORK_AREA))
    else:
        area = work_area(
            cast(int, values["display_width"]), cast(int, values["display_height"]), scale, cast(int, values[_WINDOWS])
        )
    viewport = (cast(int, values["viewport_width"]), cast(int, values["viewport_height"]))
    state = values.get("window_state") or ("maximised" if viewport == maximised_viewport(area, scale) else "normal")
    window = area if state == "maximised" else normal_window_around(*viewport, area, scale)
    worked_out = {
        **dict(zip(_WORK_AREA, area, strict=True)),
        "window_state": state,
        **dict(zip(_WINDOW, window, strict=True)),
    }
    return {name: value for name, value in worked_out.items() if name not in values}


def _fits(row: Mapping[str, object], values: Mapping[str, object]) -> bool:
    """Return whether a row of the table agrees with each value it has that is set already."""
    return all(name == _WEIGHT or name not in values or values[name] == value for name, value in row.items())


def _any_fits(rows: Sequence[Mapping[str, object]], values: Mapping[str, object]) -> bool:
    return any(_fits(row, values) for row in rows)


def _point(seed: int, group: str) -> int:
    """Return a whole number for drawing a group from a seed, which is the same on every Host and in every Python."""
    return int.from_bytes(hashlib.sha256(f"mitto machine {seed} {group}".encode()).digest())


def _seed_of(values: Mapping[str, object]) -> int:
    """Return a seed for drawing what a saved Machine with no seed lacks, from what it has, the same in every process."""
    held = json.dumps(values, sort_keys=True, separators=(",", ":"))
    return int.from_bytes(hashlib.sha256(f"mitto saved machine {held}".encode()).digest()[: _SEED_BITS // 8])


def _weighted_choice(rows: Sequence[Mapping[str, object]], point: int) -> dict[str, object]:
    """Return the values of the row that `point` lands on, where each row takes as many of the points as its weight."""
    weights = [cast(int, row[_WEIGHT]) for row in rows]
    landing = point % sum(weights)
    for row, weight in zip(rows, weights, strict=True):
        if landing < weight:
            return {name: value for name, value in row.items() if name != _WEIGHT}
        landing -= weight
    raise AssertionError("the point lands past the last row")
