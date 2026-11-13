"""Tests of animation frame callbacks in a Page: requestAnimationFrame(), webkitRequestAnimationFrame() and
cancelAnimationFrame(), with the frames that the Page begins at its Machine's refresh rate.

The tests of the Page program run where MITTO_PAGE_PROGRAM names it, as tests/test_page_events.py does, at the refresh
rate of 60 Hz that the program has without a Session. Those of a Session need a native library with the Page program
beside it, as tests/test_page.py does. What they hold a Page to is what Chrome 157.0.8103.51 gave for the same scripts.
Times are checked against each other, never against the wall clock, since a busy runner can make a frame late.
"""

import itertools
import json
import os
import subprocess
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

# The refresh rate of the Page program without a Session.
_DEFAULT_REFRESH_RATE = 60

# How far the clamping of 100 microseconds can take a time from the one it coarsens, either way.
_CLAMPING_MS = 0.1

# How long after its vsync a frame starts, which the Page takes from Chrome, and how much later than a callback's end
# the Page can pick the next frame, beside the clamping of both times.
_BEGIN_FRAME_LATENCY_MS = 1.2
_SLACK_MS = 0.5


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> Any:
    """Return what the script's result, written as JSON by the script itself, holds."""
    result = subprocess.run(
        [program, "-"],
        input=f"Promise.resolve(({script})).then(value => JSON.stringify(value))",
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def _period_ms(refresh_rate: int) -> float:
    """Return the period between two vsyncs that Chrome works out from a refresh rate: whole microseconds."""
    return round(1_000_000 / refresh_rate) / 1000


def _is_whole_periods(difference: float, period: float) -> bool:
    """Return whether two times of frames are a whole number of periods apart, give or take the clamping of each."""
    periods = round(difference / period)
    return periods >= 1 and abs(difference - periods * period) < 2 * _CLAMPING_MS


def _assert_frames_a_period_apart(frames: list[list[float]], period: float) -> None:
    """Assert that frames come at the vsyncs of a period, from what each callback got and when it ended.

    Each frame's time is a whole number of periods after the last. A frame whose callback ended before the BeginFrame
    of the next vsync reached the compositor, which is a frame's start after that vsync, is followed by the frame of
    that vsync. One that ended later, as when a busy runner wakes the Page late, misses vsyncs, as a busy Chrome does,
    so its successor is only held to whole periods.
    """
    times = [time for time, _ in frames]
    differences = [later - earlier for earlier, later in itertools.pairwise(times)]

    assert all(_is_whole_periods(difference, period) for difference in differences), differences
    assert all(round(time * 10) / 10 == time for time in times)
    deadline = period + _BEGIN_FRAME_LATENCY_MS - _SLACK_MS
    in_time = [later - time for (time, end), later in zip(frames, times[1:], strict=False) if end - time < deadline]
    assert all(abs(difference - period) < 2 * _CLAMPING_MS for difference in in_time), (frames, period)


# Records the time that each of a run of animation frame callbacks gets, each requested from the one before, and when
# the callback ended. It starts a while after the document's zero, which a frame's time counts from: a frame that began
# before it gets 0.
_FRAMES_SCRIPT = """
  new Promise((resolve) => {
    const frames = [];
    const loop = (time) => {
      if (frames.length + 1 < %d) {
        requestAnimationFrame(loop);
        frames.push([time, performance.now()]);
      } else {
        frames.push([time, performance.now()]);
        resolve(frames);
      }
    };
    setTimeout(() => requestAnimationFrame(loop), 50);
  })
"""


def test_animation_frame_callbacks_run_as_chrome_runs_them(program: Path) -> None:
    # What Chrome 157 gave: IDs from 1 up, shared with webkitRequestAnimationFrame(); every callback of a frame gets
    # its time, which is a vsync before it runs and never later than when it was asked for; undefined as `this`; the
    # microtasks of each callback before the next; a callback cancelled by an earlier one of its frame does not run, and
    # one that throws is reported and stops none of the others; a callback asked for in a frame runs in the next.
    script = """
      new Promise((started) => setTimeout(started, 50)).then(() => new Promise((resolve) => {
        const error = (f) => { try { f(); return null; } catch (e) { return `${e.name}: ${e.message}`; } };
        const out = {};
        out.errors = {
          noArgs: error(() => requestAnimationFrame()),
          notFunction: error(() => requestAnimationFrame(5)),
          cancelNoArgs: error(() => cancelAnimationFrame()),
          webkitNotFunction: error(() => webkitRequestAnimationFrame({})),
        };
        out.cancels = [cancelAnimationFrame(0), cancelAnimationFrame(-1), cancelAnimationFrame("x"),
                       cancelAnimationFrame(123456), webkitCancelAnimationFrame(123456)];
        const log = [];
        const asked = performance.now();
        let second;
        const first = requestAnimationFrame(function (time) {
          'use strict';
          log.push('first');
          out.first = {time, now: performance.now(), thisIsUndefined: this === undefined, args: arguments.length};
          Promise.resolve().then(() => log.push('microtask of first'));
          cancelAnimationFrame(second);
          requestAnimationFrame((next) => {
            log.push('next');
            out.next = next;
            out.log = log;
            resolve(out);
          });
        });
        second = requestAnimationFrame(() => log.push('second'));
        const third = requestAnimationFrame(function (time) {
          log.push('third');
          out.third = {time, thisIsWindow: this === window};
          throw new Error('from an animation frame callback');
        });
        const legacy = webkitRequestAnimationFrame((time) => {
          log.push('legacy');
          out.legacy = time;
        });
        const cancelled = requestAnimationFrame(() => log.push('cancelled'));
        webkitCancelAnimationFrame(cancelled);
        const fourth = requestAnimationFrame((time) => { log.push('fourth'); out.fourth = time; });
        out.ids = [first, second, third, legacy, cancelled, fourth];
        out.asked = asked;
        out.timeOrigin = performance.timeOrigin;
      }))
    """
    result = subprocess.run(
        [program, "-"],
        input=f"Promise.resolve(({script})).then(value => JSON.stringify(value))",
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert result.returncode == 0
    assert result.stderr == "Uncaught Error: from an animation frame callback\n"
    out = json.loads(result.stdout)

    assert out["errors"] == {
        "noArgs": "TypeError: Failed to execute 'requestAnimationFrame' on 'Window': 1 argument required, but only 0 "
        "present.",
        "notFunction": "TypeError: Failed to execute 'requestAnimationFrame' on 'Window': parameter 1 is not of type "
        "'Function'.",
        "cancelNoArgs": "TypeError: Failed to execute 'cancelAnimationFrame' on 'Window': 1 argument required, but only 0 "
        "present.",
        "webkitNotFunction": "TypeError: Failed to execute 'webkitRequestAnimationFrame' on 'Window': parameter 1 is not "
        "of type 'Function'.",
    }
    assert out["cancels"] == [None] * 5
    assert out["ids"] == [1, 2, 3, 4, 5, 6]
    assert out["log"] == ["first", "microtask of first", "third", "legacy", "fourth", "next"]
    first = out["first"]
    assert (first["thisIsUndefined"], first["args"], out["third"]["thisIsWindow"]) == (True, 1, True)
    # The frame that a request starts is a missed one, with the time of the vsync before it.
    assert out["asked"] - _period_ms(_DEFAULT_REFRESH_RATE) - 2 * _CLAMPING_MS < first["time"]
    assert first["time"] < out["asked"] + 2 * _CLAMPING_MS
    assert first["time"] < first["now"] + 2 * _CLAMPING_MS
    assert out["third"]["time"] == out["fourth"] == first["time"]
    # The frame's time is coarsened from the document's zero, to whole tenths of a millisecond.
    assert round(first["time"] * 10) / 10 == first["time"]
    # A legacy callback gets that time on the wall clock, as precise as the clock is.
    assert abs(out["legacy"] - (out["timeOrigin"] + first["time"])) < 1
    assert _is_whole_periods(out["next"] - first["time"], _period_ms(_DEFAULT_REFRESH_RATE))


def test_frames_come_a_vsync_apart_at_60_hz_without_a_session(program: Path) -> None:
    # Chrome gave 16.6 and 16.7 at 60 Hz.
    _assert_frames_a_period_apart(_evaluate(program, _FRAMES_SCRIPT % 30), _period_ms(_DEFAULT_REFRESH_RATE))


def test_idle_periods_end_with_each_frame_and_are_long_again_once_frames_stop(program: Path) -> None:
    # While frames come, an idle period lasts at most until the next vsync, and once they stop, up to 50 milliseconds.
    script = """
      new Promise((resolve) => {
        const during = [];
        let frames = 0;
        const loop = () => {
          requestIdleCallback((deadline) => during.push(deadline.timeRemaining()));
          if (++frames < 10) {
            requestAnimationFrame(loop);
            return;
          }
          setTimeout(() => requestIdleCallback((deadline) => resolve({during, after: deadline.timeRemaining()})), 100);
        };
        requestAnimationFrame(loop);
      })
    """
    out = _evaluate(program, script)

    assert out["during"]
    assert max(out["during"]) < _period_ms(_DEFAULT_REFRESH_RATE) + 2 * _CLAMPING_MS
    assert out["after"] > _period_ms(_DEFAULT_REFRESH_RATE) + 2 * _CLAMPING_MS


def test_the_command_line_waits_for_a_callback_and_a_cancelled_one_keeps_it_waiting_no_longer(program: Path) -> None:
    script = """
      new Promise((resolve) => {
        cancelAnimationFrame(requestAnimationFrame(() => {}));
        requestAnimationFrame(() => resolve('ran'));
      })
    """

    assert _evaluate(program, script) == "ran"


# A Page of a Session


@pytest.fixture
def page_program(native_library: Any, both_apis: str) -> None:
    """Run a test through each API in turn, or skip it if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture(params=[24, 60, 75, 144, 165, 240, 1000])
def machine_page(request: pytest.FixtureRequest, page_program: None) -> Iterator[tuple[mitto.Machine, Page]]:
    machine = mitto.Machine.draw(seed=4, refresh_rate=request.param)
    with mitto.Session(machine=machine) as session, session._open_page("about:blank") as page:  # pyright: ignore[reportPrivateUsage]
        yield machine, page


def test_frames_come_at_the_machines_refresh_rate(machine_page: tuple[mitto.Machine, Page]) -> None:
    # Even a display faster than a frame takes to start, such as one of 1000 Hz, gets the next vsync's frame.
    machine, page = machine_page
    _assert_frames_a_period_apart(page.evaluate(_FRAMES_SCRIPT % 60), _period_ms(machine.refresh_rate))
