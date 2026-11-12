"""Tests of matchMedia() in a Page: the media queries of native/css and the MediaQueryList of native/dom.

The cases of Blink's own media_query_set_test.cc and media_query_evaluator_test.cc run against the parser and the
evaluator in media_query_test, which CI's native job runs on every Host. Here the Page program parses and writes back
what Chrome 157 wrote back for the same queries, and a Page on a Session answers each media feature from the Session's
Machine, as Chrome on Windows answers it on that machine.

The tests of the Page program run where MITTO_PAGE_PROGRAM names it, as tests/test_page_document.py does; those of a
Session need a native library with the Page program beside it, as tests/test_page.py does.
"""

import json
import math
import os
import subprocess
from collections.abc import Callable, Iterator
from fractions import Fraction
from functools import partial
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

# Queries and what Chrome 157 writes back for each in MediaQueryList.media, which follows no machine: lower case,
# Chrome's spacing, values as Blink writes them, math simplified, "not all" for a query that does not parse, and
# <general-enclosed> as it was given.
_WRITTEN_BACK = [
    ("", ""),
    ("screen AND (MIN-WIDTH:500px)", "screen and (min-width: 500px)"),
    ("all and (min-width:/*bla*/500px)", "(min-width: 500px)"),
    ("only screen and (orientation:LANDSCAPE)", "only screen and (orientation: landscape)"),
    ("not print", "not print"),
    ("(min-resolution: 0.83333333333333333333dppx)", "(min-resolution: 0.833333dppx)"),
    ("(resolution: calc(96dpi))", "(resolution: calc(1dppx))"),
    ("(resolution: calc(1x + 2x))", "(resolution: calc(3dppx))"),
    ("(width: calc(1px + 1em))", "(width: calc(1em + 1px))"),
    ("(width: calc(2 * (1px + 1em)))", "(width: calc(2em + 2px))"),
    ("(width: calc(1in))", "(width: calc(96px))"),
    ("(width: min(1px, 2em))", "(width: min(1px, 2em))"),
    ("(width: clamp(none, 2px, 3px))", "(width: calc(2px))"),
    ("(width: calc(infinity * 1px))", "(width: calc(infinity * 1px))"),
    ("(min-color: 4294967296)", "(min-color: 2147483647)"),
    ("(device-aspect-ratio: 16.1/9.0)", "(device-aspect-ratio: 16.1 / 9)"),
    ("(aspect-ratio: 0/0)", "(aspect-ratio: 1 / 0)"),
    # Blink checks no unit for a ratio's first term.
    ("(aspect-ratio: 1px)", "(aspect-ratio: 1px / 1)"),
    ("(min-aspect-ratio: calc(1.4))", "(min-aspect-ratio: calc(1.4) / 1)"),
    ("(width: 123456789px)", "(width: 1.23457e+08px)"),
    ("(100px<width<=200px)", "(100px < width <= 200px)"),
    ("(width >= 100px) or (not (color))", "(width >= 100px) or (not (color))"),
    ("(m\\61x-width: 300px)", "(max-width: 300px)"),
    ("(max-width: 50%)", "(max-width: 50%)"),
    ("(min-orientation: portrait)", "(min-orientation: portrait)"),
    ("screen and (width: 100px) or (width: 200px)", "not all"),
    (",screen,,&invalid", "not all, screen, not all, not all"),
    ("(unknown: 50kg)", "(unknown: 50kg)"),
    ("(color) and /* mid */ (unknown: 1)", "(color) and (unknown: 1)"),
    ("(color", "(color)"),
    # Experimental in Chrome 157, so <general-enclosed>.
    ("(inverted-colors: inverted)", "(inverted-colors: inverted)"),
    ("(prefers-reduced-data: reduce)", "(prefers-reduced-data: reduce)"),
]

# The media features whose values are the same on every Machine, as Chrome on Windows gives them in a tab: a display of 8
# bits a channel in sRGB and standard dynamic range, a mouse, and a window of one segment.
_EVERY_MACHINE = {
    "(color)": True,
    "(color: 8)": True,
    "(min-color: 9)": False,
    "(monochrome)": False,
    "(color-index)": False,
    "(color-gamut: srgb)": True,
    "(color-gamut: p3)": False,
    "(dynamic-range: standard)": True,
    "(dynamic-range: high)": False,
    "(pointer: fine)": True,
    "(hover: hover)": True,
    "(any-hover: hover)": True,
    "(any-hover: none)": False,
    "(any-pointer: fine)": True,
    "(display-mode: browser)": True,
    "(scripting: enabled)": True,
    "(update: fast)": True,
    "(overflow-block: scroll)": True,
    "(overflow-inline: scroll)": True,
    "(grid: 0)": True,
    "(-webkit-transform-3d)": True,
    "(horizontal-viewport-segments: 1)": True,
    "(vertical-viewport-segments: 1)": True,
    "(device-posture: continuous)": True,
    "(scan)": False,
    "screen": True,
    "print": False,
    "not print": True,
    "(inverted-colors)": False,
    "(inverted-colors: none)": False,
    "not (inverted-colors)": False,
    "(prefers-reduced-data: no-preference)": False,
}


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str, *options: str) -> Any:
    result = subprocess.run(
        [program, *options, script], capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def test_a_query_is_written_back_as_chrome_writes_it(program: Path) -> None:
    queries = [query for query, _ in _WRITTEN_BACK]
    script = f"JSON.stringify({json.dumps(queries)}.map(query => matchMedia(query).media))"

    assert dict(zip(queries, _evaluate(program, script), strict=True)) == dict(_WRITTEN_BACK)


def test_a_media_query_list_is_an_event_target_whose_listeners_never_hear_a_change(program: Path) -> None:
    script = """
      const list = matchMedia("(color)");
      let heard = 0;
      const listener = () => heard++;
      list.addListener(listener);
      list.addListener(null);
      list.addEventListener("change", listener);
      list.removeListener(listener);
      list.addListener(listener);
      JSON.stringify([
        list instanceof MediaQueryList, list instanceof EventTarget, list !== matchMedia("(color)"), heard,
        String(matchMedia), matchMedia.length,
      ])
    """

    assert _evaluate(program, script) == [True, True, True, 0, "function matchMedia() { [native code] }", 1]


def test_a_page_makes_its_own_media_query_list_events(program: Path) -> None:
    script = """
      const event = new MediaQueryListEvent("change", {media: "(color)", matches: true, bubbles: true});
      const empty = new MediaQueryListEvent("change");
      JSON.stringify([event.type, event.media, event.matches, event.bubbles, event instanceof Event, empty.media,
                      empty.matches])
    """

    assert _evaluate(program, script) == ["change", "(color)", True, True, True, "", False]


def test_a_page_takes_as_many_nested_parentheses_as_chrome(program: Path) -> None:
    # Blink's parser recurses once for each parenthesis of a condition, and Chrome 157 takes 20,000 of them. The
    # innermost is <general-enclosed>, and Blink closes each of the others when it writes them back.
    depth = 5000
    script = f"JSON.stringify(matchMedia('('.repeat({depth})).media === '('.repeat({depth}) + ')'.repeat({depth - 1}))"

    assert _evaluate(program, script) is True


def test_a_feature_parses_as_the_pages_runtime_enabled_features_say(program: Path) -> None:
    script = "JSON.stringify(['(inverted-colors: none)', '(inverted-colors)'].map(query => matchMedia(query).matches))"

    assert _evaluate(program, script) == [False, False]
    assert _evaluate(program, script, "--enable-blink-features=InvertedColors") == [True, False]


@pytest.fixture
def page_program(native_library: Any, both_apis: str) -> None:
    """Run a test through each API in turn, or skip it if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


# Machines that differ in what a page reads: one drawn, a dark one with a touch screen and reduced motion and
# transparency on a display at 125%, and one with a contrast theme.
_MACHINES: dict[str, Callable[[], mitto.Machine]] = {
    "drawn": partial(mitto.Machine.draw, seed=1),
    "dark-touch-125": partial(
        mitto.Machine.draw,
        seed=2,
        display_width=1920,
        display_height=1080,
        scale=1.25,
        color_scheme="dark",
        reduced_motion="reduce",
        reduced_transparency="reduce",
        touch_points=10,
    ),
    "contrast-theme": partial(mitto.Machine.draw, seed=3, contrast_theme="desert"),
}


@pytest.fixture(params=sorted(_MACHINES))
def machine_page(request: pytest.FixtureRequest, page_program: None) -> Iterator[tuple[mitto.Machine, Page]]:
    machine = _MACHINES[request.param]()
    with mitto.Session(machine=machine) as session, session._open_page("about:blank") as page:  # pyright: ignore[reportPrivateUsage]
        yield machine, page


def _expected_from(machine: mitto.Machine) -> dict[str, bool]:
    """What Chrome on Windows answers on `machine`, from its fields alone."""
    scale = Fraction(machine.scale)
    screen_width = math.ceil(machine.display_width / scale)
    screen_height = math.ceil(machine.display_height / scale)
    contrast_theme = machine.contrast_theme != "none"
    return {
        f"(width: {machine.viewport_width}px)": True,
        f"(width: {machine.viewport_width + 1}px)": False,
        f"(height: {machine.viewport_height}px)": True,
        f"(min-height: {machine.viewport_height + 1}px)": False,
        f"(device-width: {screen_width}px)": True,
        f"(device-height: {screen_height}px)": True,
        f"(aspect-ratio: {machine.viewport_width}/{machine.viewport_height})": True,
        f"(device-aspect-ratio: {screen_width}/{screen_height})": True,
        "(orientation: landscape)": machine.viewport_width > machine.viewport_height,
        f"(resolution: {machine.scale}dppx)": True,
        f"(-webkit-device-pixel-ratio: {machine.scale})": True,
        # Blink takes a math function of a number as an integer first, which rounds it.
        "(-webkit-device-pixel-ratio: calc(1.4))": machine.scale == 1,
        # A ratio's first term may have a unit, which Blink drops.
        "(min-aspect-ratio: 1px)": machine.viewport_width >= machine.viewport_height,
        "(min-resolution: 2dppx)": machine.scale >= 2,
        "(prefers-color-scheme: dark)": machine.color_scheme == "dark",
        "(prefers-color-scheme: light)": machine.color_scheme == "light",
        "(prefers-reduced-motion: reduce)": machine.reduced_motion == "reduce",
        "(prefers-reduced-motion)": machine.reduced_motion == "reduce",
        "(prefers-reduced-transparency: reduce)": machine.reduced_transparency == "reduce",
        "(forced-colors: active)": contrast_theme,
        "(forced-colors: none)": not contrast_theme,
        "(prefers-contrast: more)": contrast_theme,
        "(prefers-contrast: no-preference)": not contrast_theme,
        "(any-pointer: coarse)": machine.touch_points > 0,
        "(min-width: 40em)": machine.viewport_width >= 640,
        "(max-width: 100vw)": True,
        **_EVERY_MACHINE,
    }


def test_a_page_answers_each_media_feature_from_its_machine(machine_page: tuple[mitto.Machine, Page]) -> None:
    machine, page = machine_page
    expected = _expected_from(machine)
    queries = list(expected)

    matches = page.evaluate(f"{json.dumps(queries)}.map(query => matchMedia(query).matches)")

    assert dict(zip(queries, matches, strict=True)) == expected
