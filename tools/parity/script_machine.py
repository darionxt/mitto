"""What a Page gives of its Machine at each place that the Normal Form of a Script Fingerprint hides as <machine>.

A Parity Check of a Script Fingerprint compares where those places are with the Baseline, and holds what a Page gave
there to its Machine instead: the value that Chrome gives for that Machine, where a Page reads it from its start
message, or else the form that real Chrome's values have, in tools/capture/machine_forms.py. docs/parity.md, "The
Machine's values", says why.
"""

from collections.abc import Callable, Iterator, Mapping
from fractions import Fraction
from typing import Final

from mitto import Machine
from mitto._machine import effective_connection_type, rounded_downlink, rounded_rtt
from mitto._window import screen_size
from tools.capture.machine_forms import SCRIPT_FORMS
from tools.capture.probe import RecordValue
from tools.capture.script_normal_form import (
    DATE_STRING_NAMES,
    DATE_STRINGS,
    DATE_TIME_FORMAT,
    DATE_TIME_FORMAT_HOURS,
    HIGH_ENTROPY_VALUES,
    MEDIA_AGREEMENT,
    MEDIA_INTEGERS,
    MEDIA_KEYWORDS,
    TIME_ZONE_OFFSETS,
    Computed,
    Getter,
    Place,
    machine_values,
)
from tools.parity.script_gaps import same_json

__all__ = ["NOISED", "PAGE_READS", "WORKED_OUT_IN_A_PAGE", "machine_misses"]


def _screen(machine: Machine) -> tuple[int, int]:
    return screen_size(machine.display_width, machine.display_height, Fraction(machine.scale))


def _keyword(value: str) -> list[RecordValue]:
    return [value]


PAGE_READS: Final[Mapping[Place, Callable[[Machine], RecordValue]]] = {
    Getter("globalThis", "innerWidth"): lambda machine: machine.viewport_width,
    Getter("globalThis", "innerHeight"): lambda machine: machine.viewport_height,
    Getter("globalThis", "outerWidth"): lambda machine: machine.window_width,
    Getter("globalThis", "outerHeight"): lambda machine: machine.window_height,
    Getter("globalThis", "screenX"): lambda machine: machine.window_x,
    Getter("globalThis", "screenY"): lambda machine: machine.window_y,
    Getter("globalThis", "screenLeft"): lambda machine: machine.window_x,
    Getter("globalThis", "screenTop"): lambda machine: machine.window_y,
    Getter("globalThis", "devicePixelRatio"): lambda machine: machine.scale,
    Getter("screen", "availWidth"): lambda machine: machine.work_area_width,
    Getter("screen", "availHeight"): lambda machine: machine.work_area_height,
    Getter("screen", "width"): lambda machine: _screen(machine)[0],
    Getter("screen", "height"): lambda machine: _screen(machine)[1],
    Getter("screen", "availLeft"): lambda machine: machine.work_area_x,
    Getter("screen", "availTop"): lambda machine: machine.work_area_y,
    Getter("visualViewport", "width"): lambda machine: machine.viewport_width,
    Getter("visualViewport", "height"): lambda machine: machine.viewport_height,
    Getter('viewport.segments["0"]', "width"): lambda machine: machine.viewport_width,
    Getter('viewport.segments["0"]', "height"): lambda machine: machine.viewport_height,
    Getter('viewport.segments["0"]', "right"): lambda machine: machine.viewport_width,
    Getter('viewport.segments["0"]', "bottom"): lambda machine: machine.viewport_height,
    Getter("navigator", "hardwareConcurrency"): lambda machine: machine.processors,
    Getter("navigator", "deviceMemory"): lambda machine: machine.memory,
    Getter("navigator", "maxTouchPoints"): lambda machine: machine.touch_points,
    Getter("navigator.connection", "effectiveType"): lambda machine: effective_connection_type(machine.round_trip_ms),
    # Before the renderer's noise, which keeps a value in the same form (NOISED).
    Getter("navigator.connection", "rtt"): lambda machine: rounded_rtt(machine.round_trip_ms),
    Getter("navigator.connection", "downlink"): lambda machine: rounded_downlink(machine.throughput_kbps),
    Computed(HIGH_ENTROPY_VALUES, ("platformVersion",)): lambda machine: machine.platform_version,
    Computed(MEDIA_KEYWORDS, ("prefers-color-scheme",)): lambda machine: _keyword(machine.color_scheme),
    Computed(MEDIA_KEYWORDS, ("prefers-reduced-motion",)): lambda machine: _keyword(machine.reduced_motion),
    Computed(MEDIA_KEYWORDS, ("prefers-reduced-transparency",)): lambda machine: _keyword(machine.reduced_transparency),
    # Each contrast theme that a Machine takes gives a contrast of at least 7:1 (docs/persona.md).
    Computed(MEDIA_KEYWORDS, ("forced-colors",)): lambda machine: _keyword(
        "none" if machine.contrast_theme == "none" else "active"
    ),
    Computed(MEDIA_KEYWORDS, ("prefers-contrast",)): lambda machine: _keyword(
        "no-preference" if machine.contrast_theme == "none" else "more"
    ),
    # Chrome 155 to 157 do not parse it, so it matches nothing, whatever the Machine's inverted_colors (#218).
    Computed(MEDIA_KEYWORDS, ("inverted-colors",)): lambda _: [],
    Computed(MEDIA_KEYWORDS, ("orientation",)): lambda machine: _keyword(
        "portrait" if machine.viewport_height >= machine.viewport_width else "landscape"
    ),
    Computed(DATE_TIME_FORMAT, ("timeZone",)): lambda machine: machine.time_zone,
    Computed(DATE_TIME_FORMAT_HOURS, ("timeZone",)): lambda machine: machine.time_zone,
    # A Machine claims one display, with no HDR, a gamut of sRGB, and no rotation.
    Getter("screen", "colorDepth"): lambda _: 24,
    Getter("screen", "pixelDepth"): lambda _: 24,
    Getter("screen", "isExtended"): lambda _: False,
    Getter("screen.orientation", "angle"): lambda _: 0,
    Getter("screen.orientation", "type"): lambda machine: (
        "portrait-primary" if _screen(machine)[1] >= _screen(machine)[0] else "landscape-primary"
    ),
    Computed(MEDIA_KEYWORDS, ("color-gamut",)): lambda _: ["srgb"],
    Computed(MEDIA_KEYWORDS, ("dynamic-range",)): lambda _: ["standard"],
    Computed(MEDIA_INTEGERS, ("color",)): lambda _: [8],
}
"""What a page reads of a Machine at each place that the Normal Form hides as <machine>, as Chrome gives it there, which
a Page gets in its start message (SessionState._page_start())."""

NOISED: Final = frozenset({Getter("navigator.connection", "rtt"), Getter("navigator.connection", "downlink")})
"""The places of PAGE_READS whose value has the renderer's noise for the page's host on it, so that only its form says
whether it is the Machine's."""

WORKED_OUT_IN_A_PAGE: Final = frozenset(
    {
        # From the memory, by V8's heap limits and Blink's rounding.
        Getter("console.memory", "jsHeapSizeLimit"),
        Getter("performance.memory", "jsHeapSizeLimit"),
        # From the touch points and the window, by Blink's media queries, or not at all where they are not parsed.
        *(Computed(MEDIA_KEYWORDS, (name,)) for name in ("pointer", "any-pointer", "hover", "any-hover")),
        Computed(MEDIA_KEYWORDS, ("display-state",)),
        Computed(MEDIA_KEYWORDS, ("video-dynamic-range",)),
        # From the time zone, by V8 and ICU.
        Computed(TIME_ZONE_OFFSETS),
        *(Computed(DATE_STRINGS, (day, name)) for day in ("winter", "summer") for name in DATE_STRING_NAMES),
        # From the viewport, the screen and the scale, by Blink's media queries.
        *(
            Computed(MEDIA_AGREEMENT, (name,))
            for name in (
                "width = innerWidth",
                "height = innerHeight",
                "aspect-ratio = innerWidth / innerHeight",
                "device-width = screen.width",
                "device-height = screen.height",
                "device-aspect-ratio = screen.width / screen.height",
            )
        ),
        # From the processor's model, which a Machine does not hold yet.
        Getter("navigator", "cpuPerformance"),
    }
)
"""The places of the machine whose value a Page works out by Chrome's own code from its start message, or cannot give
yet, which a Parity Check holds to the form of real Chrome's values instead."""

THE_MACHINES = "<machine>: the Machine's"
NOT_THE_MACHINES = "<machine>: not the Machine's"


def machine_misses(record: dict[str, RecordValue], machine: Machine) -> Iterator[tuple[Place, str, str]]:
    """Yield each place of the machine at which a Page's record does not give what Chrome gives for its Machine.

    Each comes with what Chrome gives there and what the Page gave, said without the value, since a Session draws a
    new Machine each time: "<machine>: the Machine's" and "<machine>: not the Machine's", or for a place that only a
    form can judge, the form's description and "<machine>: not of that form". A getter that threw is left out, since
    the Normal Form keeps it and the comparison shows it.

    Args:
        record: What the probe gave in the Page, before its Normal Form.
        machine: The Machine of the Page's Session.
    """
    for place, value in machine_values(record):
        if isinstance(value, list) and len(value) == 2 and value[0] == "throws":
            continue
        read = PAGE_READS.get(place)
        if read is not None and place not in NOISED:
            if not same_json(value, read(machine)):
                yield place, THE_MACHINES, NOT_THE_MACHINES
            continue
        form = SCRIPT_FORMS[place]
        if not form.fits(value):
            yield place, f"<machine>: {form.description}", "<machine>: not of that form"
