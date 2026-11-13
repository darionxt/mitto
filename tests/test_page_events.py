"""Tests of a Page's event loop: tasks, microtasks, timers, idle callbacks, window.event and the coarsened clocks, in the
Page program.

These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_document.py does. The program runs a script
from standard input, which keeps long scripts and any text intact on every Host, then its event loop until no task or
timer is left, and prints what the promise that the script returns was fulfilled with. Orders are exact. Times are only
checked from below, since a busy runner can make any timer late.
"""

import json
import os
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_SCRIPT_THREW = 1


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, "-"],
        input=script,
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )


def _evaluate(program: Path, script: str) -> Any:
    """Return what the script's result, written as JSON by the script itself, holds."""
    result = _run(program, f"Promise.resolve(({script})).then(value => JSON.stringify(value))")
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def test_tasks_microtasks_and_timers_run_in_the_order_of_the_html_standard(program: Path) -> None:
    # A microtask runs before any task, queueMicrotask() and promise jobs share one queue, and timers of the same
    # timeout run in the order they were set. setTimeout(f, 1) runs after setTimeout(g, 0), as Chrome has no clamp to 1
    # millisecond.
    script = """
      new Promise(resolve => {
        const log = [];
        setTimeout(() => log.push("timeout 1"), 1);
        setTimeout(() => log.push("timeout 0"), 0);
        setTimeout(() => {
          log.push("timeout 0 again");
          Promise.resolve().then(() => log.push("microtask of a task"));
          queueMicrotask(() => log.push("queued in a task"));
        }, 0);
        Promise.resolve().then(() => log.push("promise job"));
        queueMicrotask(() => log.push("queued microtask"));
        log.push("script");
        setTimeout(() => resolve(log), 20);
      })
    """

    assert _evaluate(program, script) == [
        "script",
        "promise job",
        "queued microtask",
        "timeout 0",
        "timeout 0 again",
        "microtask of a task",
        "queued in a task",
        "timeout 1",
    ]


def test_the_html_standards_example_of_converting_a_handler_runs_one_then_two(program: Path) -> None:
    # The HTML Standard's example: the handler's conversion to a string sets the first timer.
    script = """
      new Promise(resolve => {
        var log = '';
        function logger(s) { log += s + ' '; }
        globalThis.logger = logger;
        setTimeout({ toString: function () {
          setTimeout("logger('ONE')", 100);
          return "logger('TWO')";
        } }, 100);
        setTimeout(() => resolve(log), 150);
      })
    """

    assert _evaluate(program, script) == "ONE TWO "


def test_the_html_standards_example_of_back_to_back_work_stops_when_it_clears_its_timer(program: Path) -> None:
    script = """
      new Promise(resolve => {
        let chunks = 0;
        function doExpensiveWork() { return ++chunks === 5; }
        function rescheduleWork() {
          const id = setTimeout(rescheduleWork, 0);
          if (doExpensiveWork()) {
            clearTimeout(id);
            setTimeout(() => resolve(chunks), 30);
          }
        }
        function scheduleWork() { setTimeout(rescheduleWork, 0); }
        scheduleWork();
      })
    """

    assert _evaluate(program, script) == 5


def test_the_html_standards_examples_of_queue_microtask_batch_without_a_task_in_between(program: Path) -> None:
    script = """
      new Promise(resolve => {
        const sent = [];
        const queuedToSend = [];
        function sendData(data) {
          queuedToSend.push(data);
          if (queuedToSend.length === 1) {
            queueMicrotask(() => {
              sent.push(JSON.stringify(queuedToSend));
              queuedToSend.length = 0;
            });
          }
        }
        sendData("a");
        sendData("b");
        setTimeout(() => sendData("c"), 0);
        setTimeout(() => resolve(sent), 10);
      })
    """

    assert _evaluate(program, script) == ['["a","b"]', '["c"]']


def test_nested_timers_are_clamped_to_4_milliseconds_from_the_seventh_on(program: Path) -> None:
    # Each step records how long its timer of 0 waited. The HTML Standard clamps a timer that a timer of nesting level
    # 5 or more sets: the seventh of a chain and after.
    script = """
      new Promise(resolve => {
        const waits = [];
        let last = performance.now();
        function step() {
          const now = performance.now();
          waits.push(now - last);
          last = now;
          if (waits.length < 30) { setTimeout(step, 0); } else { resolve(waits); }
        }
        setTimeout(step, 0);
      })
    """

    waits = _evaluate(program, script)

    assert len(waits) == 30
    # Each reading of performance.now() is coarsened to 100 microseconds, up or down, so two readings can lose up to
    # 0.2 ms of a wait of 4 ms, and their difference in doubles can fall just below a multiple of 0.1.
    assert all(wait >= 3.8 for wait in waits[6:])


def test_an_interval_repeats_until_cleared_and_is_clamped_once_deep_enough(program: Path) -> None:
    script = """
      new Promise(resolve => {
        const times = [];
        const id = setInterval((a, b) => {
          times.push([performance.now(), a + b]);
          if (times.length === 20) { clearInterval(id); resolve(times); }
        }, 0, 1, 2);
      })
    """

    times = _evaluate(program, script)

    assert [argument for _, argument in times] == [3] * 20
    # From the seventh run on, the interval is at least 4 milliseconds, so the last thirteen take at least 48.
    assert times[-1][0] - times[6][0] >= 4 * 13 - 1


def test_timer_ids_and_clearing_follow_chrome(program: Path) -> None:
    script = """
      new Promise(resolve => {
        const log = [];
        const a = setTimeout(() => log.push("a"), 0);
        const b = setTimeout(() => log.push("b"), 0);
        clearTimeout(b);
        const c = setInterval(() => { log.push("c"); clearTimeout(c); }, 0);
        clearInterval(0);
        clearTimeout(-1);
        // An empty string sets no timer.
        const empty = setTimeout("", 0);
        setTimeout(() => resolve([log, [a, b, c].every(id => id > 0), new Set([a, b, c]).size, empty]), 10);
      })
    """

    assert _evaluate(program, script) == [["a", "c"], True, 3, 0]


def test_a_negative_or_overflowing_timeout_runs_at_once(program: Path) -> None:
    # The timeout is a long: 2 ** 32 + 1 wraps to 1, and 2 ** 31 to a negative number, which counts as 0.
    script = """
      new Promise(resolve => {
        const log = [];
        setTimeout(() => log.push("wrapped"), 2 ** 31);
        setTimeout(() => log.push("negative"), -5);
        setTimeout(() => resolve(log), 20);
      })
    """

    assert _evaluate(program, script) == ["wrapped", "negative"]


def test_what_a_timer_or_a_microtask_throws_is_reported_and_the_loop_goes_on(program: Path) -> None:
    script = """
      setTimeout(() => { throw new Error("from a timer"); }, 0);
      setTimeout("throw new RangeError('from text')", 0);
      queueMicrotask(() => { throw new TypeError("from a microtask"); });
      new Promise(resolve => setTimeout(() => resolve("went on"), 10))
    """
    result = _run(program, script)

    assert (result.returncode, result.stdout) == (0, "went on\n")
    assert result.stderr.splitlines() == [
        "Uncaught TypeError: from a microtask",
        "Uncaught Error: from a timer",
        "Uncaught RangeError: from text",
    ]


def test_a_promise_that_is_rejected_or_never_settles_ends_the_command_line_with_an_error(program: Path) -> None:
    rejected = _run(program, "new Promise((resolve, reject) => setTimeout(reject, 0, new Error('no')))")
    pending = _run(program, "new Promise(() => {})")

    assert (rejected.returncode, rejected.stderr) == (_SCRIPT_THREW, "Uncaught (in promise) Error: no\n")
    assert (pending.returncode, pending.stderr) == (_SCRIPT_THREW, "the promise did not settle\n")


def test_performance_now_is_coarsened_to_a_tenth_of_a_millisecond_and_never_goes_back(program: Path) -> None:
    script = """
      (() => {
        const times = [];
        for (let i = 0; i < 2000; ++i) { times.push(performance.now()); }
        return [times, performance.timeOrigin, Date.now()];
      })()
    """
    before = time.time() * 1000

    times, time_origin, now = _evaluate(program, script)

    after = time.time() * 1000
    # Each time is a multiple of 100 microseconds, up to the rounding of a double, as Chrome's are when a page is not
    # cross-origin isolated.
    assert all(abs(value * 10 - round(value * 10)) < 1e-6 for value in times)
    assert times == sorted(times)
    # timeOrigin is a Unix time in milliseconds, coarsened too, and Date.now() an integer of milliseconds.
    assert before - 1000 <= time_origin <= after
    assert abs(time_origin * 10 - round(time_origin * 10)) < 1e-3
    assert isinstance(now, int)
    assert before - 1 <= now <= after + 1


def test_the_timers_and_queue_microtask_check_their_arguments_as_chrome_does(program: Path) -> None:
    script = """
      (() => {
        const errors = [];
        for (const f of [() => queueMicrotask(1), () => setTimeout(), () => setInterval()]) {
          try { f(); errors.push("none"); } catch (error) { errors.push(String(error)); }
        }
        return [errors, setTimeout.length, setInterval.length, clearTimeout.length, queueMicrotask.length];
      })()
    """

    assert _evaluate(program, script) == [
        [
            "TypeError: Failed to execute 'queueMicrotask' on 'Window': parameter 1 is not of type 'Function'.",
            "TypeError: Failed to execute 'setTimeout' on 'Window': 1 argument required, but only 0 present.",
            "TypeError: Failed to execute 'setInterval' on 'Window': 1 argument required, but only 0 present.",
        ],
        1,
        1,
        0,
        1,
    ]


def test_idle_callbacks_run_when_nothing_else_is_ready_as_chrome_runs_them(program: Path) -> None:
    # What Chrome 157 gave for this script once its page had loaded, when it has long idle periods as a Page does. The
    # callback with a timeout runs as a task of its own while a busy idle callback holds the idle period up.
    script = """
      new Promise((resolve) => {
        const error = (f) => { try { f(); return null; } catch (e) { return `${e.name}: ${e.message}`; } };
        const out = {};
        out.errors = {
          noArgs: error(() => requestIdleCallback()),
          notFunction: error(() => requestIdleCallback(5)),
          badOptions: error(() => requestIdleCallback(() => {}, 5)),
          cancelNoArgs: error(() => cancelIdleCallback()),
          construct: error(() => new IdleDeadline()),
        };
        out.cancels = [cancelIdleCallback(0), cancelIdleCallback(-1), cancelIdleCallback(123456)];
        const order = [];
        const first = requestIdleCallback(function (deadline) {
          order.push('idle');
          out.first = {
            thisIsWindow: this === window,
            didTimeout: deadline.didTimeout,
            remaining: deadline.timeRemaining() > 0 && deadline.timeRemaining() <= 50,
            tag: Object.prototype.toString.call(deadline),
            own: Object.getOwnPropertyNames(deadline),
            args: arguments.length,
          };
          requestIdleCallback((d) => {
            order.push('nested');
            const s = performance.now();
            while (performance.now() - s < 5) {}
            out.after = [d.didTimeout, d.timeRemaining() >= 0];
            const timed = requestIdleCallback((d2) => {
              order.push('timeout');
              out.timeout = [d2.didTimeout, d2.timeRemaining()];
              out.order = order;
              resolve(out);
            }, {timeout: 1});
            out.timedId = timed > first;
            const busy = performance.now();
            while (performance.now() - busy < 20) {}
          });
        });
        const cancelled = requestIdleCallback(() => order.push('cancelled'));
        cancelIdleCallback(cancelled);
        out.ids = [first, cancelled - first];
        setTimeout(() => order.push('timer'), 0);
      })
    """

    assert _evaluate(program, script) == {
        "errors": {
            "noArgs": "TypeError: Failed to execute 'requestIdleCallback' on 'Window': 1 argument required, but only 0 "
            "present.",
            "notFunction": "TypeError: Failed to execute 'requestIdleCallback' on 'Window': parameter 1 is not of type "
            "'Function'.",
            "badOptions": "TypeError: Failed to execute 'requestIdleCallback' on 'Window': The provided value is not of "
            "type 'IdleRequestOptions'.",
            "cancelNoArgs": "TypeError: Failed to execute 'cancelIdleCallback' on 'Window': 1 argument required, but only "
            "0 present.",
            "construct": "TypeError: Failed to construct 'IdleDeadline': Illegal constructor",
        },
        "cancels": [None, None, None],
        "ids": [1, 1],
        "first": {
            "thisIsWindow": True,
            "didTimeout": False,
            "remaining": True,
            "tag": "[object IdleDeadline]",
            "own": [],
            "args": 1,
        },
        "after": [False, True],
        "timedId": True,
        "timeout": [True, 0],
        "order": ["timer", "idle", "nested", "timeout"],
    }


def test_idle_callbacks_share_an_idle_period_that_ends_before_the_next_timer(program: Path) -> None:
    # Those posted before an idle period started share its end; one posted in it waits for the next, which starts when
    # it ends. A period lasts until the next timer is due, and at most 50 milliseconds. Chrome 157 gave all true.
    shared = """
      new Promise((resolve) => {
        const out = {};
        let first;
        requestIdleCallback((d) => {
          first = d.timeRemaining();
          const start = performance.now();
          while (performance.now() - start < 5) {}
        });
        requestIdleCallback((d) => {
          const second = d.timeRemaining();
          out.sharesThePeriod = second > 0 && second <= first - 4;
          const posted = performance.now();
          requestIdleCallback((d2) => {
            out.waitsForTheNextPeriod = performance.now() - posted >= second - 1 && d2.timeRemaining() > 30;
            resolve(out);
          });
        });
      })
    """
    timers = """
      new Promise((resolve) => {
        const out = {};
        setTimeout(() => {
          setTimeout(() => {}, 20);
          requestIdleCallback((d) => {
            out.nearTimer = d.timeRemaining() > 0 && d.timeRemaining() <= 20;
            setTimeout(() => {}, 200);
            requestIdleCallback((d2) => {
              out.farTimer = d2.timeRemaining() > 20 && d2.timeRemaining() <= 50;
              resolve(out);
            });
          });
        }, 0);
      })
    """

    assert _evaluate(program, shared) == {"sharesThePeriod": True, "waitsForTheNextPeriod": True}
    assert _evaluate(program, timers) == {"nearTimer": True, "farTimer": True}


def test_what_an_idle_callback_throws_is_reported_and_the_loop_goes_on(program: Path) -> None:
    script = """
      requestIdleCallback(() => { throw new Error("from an idle callback"); });
      new Promise(resolve => requestIdleCallback(() => resolve("went on")))
    """
    result = _run(program, script)

    assert (result.returncode, result.stdout, result.stderr) == (
        0,
        "went on\n",
        "Uncaught Error: from an idle callback\n",
    )


def test_window_event_is_the_event_being_dispatched_as_in_chrome(program: Path) -> None:
    script = """
      new Promise((resolve) => {
        const out = {};
        const d = Object.getOwnPropertyDescriptor(window, 'event');
        out.descriptor = [typeof d.get, typeof d.set, d.enumerable, d.configurable, d.get.name, d.set.name,
                          d.get.length, d.set.length];
        out.before = window.event === undefined;
        const target = new EventTarget();
        const seen = [];
        target.addEventListener('outer', (e) => {
          seen.push(['outer', window.event === e]);
          target.dispatchEvent(new Event('inner'));
          seen.push(['outer again', window.event === e]);
        });
        target.addEventListener('inner', (e) => {
          seen.push(['inner', window.event === e, window.event.type]);
          throw new Error('ignored');
        });
        target.addEventListener('outer', {handleEvent(e) { seen.push(['object', window.event === e]); }});
        target.dispatchEvent(new Event('outer'));
        out.seen = seen;
        out.after = window.event === undefined;
        const div = document.createElement('div');
        div.onclick = (e) => { out.click = [window.event === e, event === e]; };
        div.dispatchEvent(new Event('click'));
        Promise.resolve().then(() => { out.microtask = window.event; });
        setTimeout(() => {
          out.timer = window.event === undefined;
          window.event = 5;
          out.replaced = [window.event, Object.getOwnPropertyDescriptor(window, 'event').value, 'event' in window];
          resolve(out);
        }, 0);
      })
    """
    result = _run(program, f"Promise.resolve(({script})).then(value => JSON.stringify(value))")

    assert (result.returncode, result.stderr) == (0, "Uncaught Error: ignored\n")
    assert json.loads(result.stdout) == {
        "descriptor": ["function", "function", True, True, "get event", "set event", 0, 1],
        "before": True,
        "seen": [["outer", True], ["inner", True, "inner"], ["outer again", True], ["object", True]],
        "after": True,
        "click": [True, True],
        "timer": True,
        "replaced": [5, 5, True],
    }


def test_fetch_fails_on_the_command_line_which_has_no_session(program: Path) -> None:
    script = "fetch('http://127.0.0.1:1/').then(() => 'fetched', error => String(error))"

    assert _evaluate(program, script) == "TypeError: Failed to fetch"
