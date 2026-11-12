"""Where Chrome's window sits on a Windows desktop: the work area that the taskbar leaves, and Chrome's own bars.

Every size here is in device-independent pixels, which are a display's pixels over its scale, as a page reads them.
docs/persona.md, "The taskbar and Chrome's bars", says how each was read and where Chrome works each out. tools/release
writes the table of Machines with these, and a Machine whose display the table does not have takes its window from them.
"""

import math
from collections.abc import Mapping
from fractions import Fraction
from typing import NamedTuple

__all__ = [
    "READ_SCALES",
    "TASKBAR_HEIGHTS",
    "Rect",
    "first_window",
    "maximised_viewport",
    "normal_viewport",
    "normal_window_around",
    "screen_size",
    "work_area",
]


class Rect(NamedTuple):
    """A rectangle on the desktop: where its top left corner is, from the display's, and its size."""

    x: int
    y: int
    width: int
    height: int


# The taskbar of each Windows, at the bottom of the display and not hidden, as Windows sets it up. Windows 10's was read at
# 100% only, and is taken to be as high at every scale.
TASKBAR_HEIGHTS: Mapping[int, int] = {11: 48, 10: 40}

# As read on Windows 11 on 2026-11-08: Chrome's bars in a maximised window, which has no border, and the bars and top
# border, and the side borders, of a window that is not maximised.
_MAXIMISED_BARS = {Fraction(1): 87, Fraction(5, 4): 86, Fraction(3, 2): 87, Fraction(2): 87}
_NORMAL_TOP = {Fraction(1): 95, Fraction(5, 4): 95, Fraction(3, 2): 94, Fraction(2): 93}
_NORMAL_SIDES = {Fraction(1): 16, Fraction(5, 4): 14, Fraction(3, 2): 15, Fraction(2): 13}

READ_SCALES = tuple(sorted(_MAXIMISED_BARS))
"""The scales of Windows' display settings at which Chrome's bars were read. Windows offers more, which wait for a
reading."""

# Chrome's first window on a new profile, in GetDefaultWindowBounds() of chrome/browser/ui/window_sizer/window_sizer.cc,
# with the constants of window_sizer_aura.cc.
# Upstream: chrome/browser/ui/window_sizer/window_sizer.cc
_WINDOW_TILE = 10
_WINDOW_MAX_DEFAULT_WIDTH = 1050
_MIN_SCREEN_WIDTH_FOR_HALVING = 1600


def screen_size(display_width: int, display_height: int, scale: Fraction) -> tuple[int, int]:
    """Return the size of a display in pixels at a scale, as a page's screen.width and screen.height read it.

    A display whose size is no whole number of device-independent pixels takes the next one up, as Chrome encloses it.
    """
    return math.ceil(display_width / scale), math.ceil(display_height / scale)


def work_area(display_width: int, display_height: int, scale: Fraction, windows: int) -> Rect:
    """Return the work area of a display in pixels, at a scale, with the taskbar of a Windows along its bottom."""
    width, height = screen_size(display_width, display_height, scale)
    return Rect(0, 0, width, height - TASKBAR_HEIGHTS[windows])


def first_window(display_width: int, display_height: int, area: Rect) -> Rect:
    """Return Chrome's first window on a new profile, which has no saved size, on a display in pixels with this work area.

    It sits a tile in from the work area's top left corner and is two tiles lower than the work area. A display of 16:10
    or wider, whose work area is wide enough, gives it half of the width, so that two windows fit side by side, and
    otherwise it is two tiles narrower than the work area, up to a largest width. On a portrait display it is 4:3.
    """
    width = min(area.width - 2 * _WINDOW_TILE, _WINDOW_MAX_DEFAULT_WIDTH)
    height = area.height - 2 * _WINDOW_TILE
    # In floating point, as Chrome compares the display's width over its height.
    if display_width / display_height * 10 >= 16 and area.width > _MIN_SCREEN_WIDTH_FOR_HALVING:
        width = int(area.width / 2 - 1.5 * _WINDOW_TILE)
    if display_height > display_width and height > width:
        height = (width // 4) * 3
    return Rect(area.x + _WINDOW_TILE, area.y + _WINDOW_TILE, width, height)


def maximised_viewport(area: Rect, scale: Fraction) -> tuple[int, int]:
    """Return the page area of a window that fills the work area: all of its width, and its height less Chrome's bars."""
    return area.width, area.height - _reading(_MAXIMISED_BARS, scale)


def normal_viewport(window: Rect, scale: Fraction) -> tuple[int, int]:
    """Return the page area of a window that is not maximised: the window less its borders and Chrome's bars."""
    return window.width - _reading(_NORMAL_SIDES, scale), window.height - _reading(_NORMAL_TOP, scale)


def normal_window_around(viewport_width: int, viewport_height: int, area: Rect, scale: Fraction) -> Rect:
    """Return a window that is not maximised and has this page area, where Chrome puts its first window."""
    return Rect(
        area.x + _WINDOW_TILE,
        area.y + _WINDOW_TILE,
        viewport_width + _reading(_NORMAL_SIDES, scale),
        viewport_height + _reading(_NORMAL_TOP, scale),
    )


def _reading(readings: Mapping[Fraction, int], scale: Fraction) -> int:
    """Return what was read at a scale, or at the largest scale below it that was read, for a scale that was not."""
    return readings[max(read for read in readings if read <= scale)]
