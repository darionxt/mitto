"""Tests of a Page's event loop: tasks, microtasks, timers, idle callbacks, the scheduler, window.event and the coarsened
clocks, in the Page program.

These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_document.py does. The program runs a script
from standard input, which keeps long scripts and any text intact on every Host, then its event loop until no task or
timer is left, and prints what the promise that the script returns was fulfilled with. Orders are exact. Times are only
checked from below, since a busy runner can make any timer late.
"""

import json
import math
import os
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_SCRIPT_THREW = 1

# How far the clamping of 100 microseconds can take a time from the one it coarsens, either way.
_CLAMPING_MS = 0.1


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


def _idle_period_end(read: list[float]) -> tuple[float, float]:
    """Return the earliest and the latest that an idle period can end, from what one of its callbacks read in turn:
    performance.now(), timeRemaining() and performance.now() again.

    A busy runner can stop the Page between any two reads, and hold a callback up past its period's end, when it has no
    time left, which says only that the period had ended.
    """
    before, remaining, after = read
    if remaining > 0:
        return before + remaining - 2 * _CLAMPING_MS, after + remaining + 2 * _CLAMPING_MS
    return -math.inf, after + 2 * _CLAMPING_MS


def test_idle_callbacks_share_an_idle_period_that_ends_before_the_next_timer(program: Path) -> None:
    # Those posted before an idle period started share its end; one posted in it waits for the next, which starts when
    # it ends. A period lasts until the next timer is due, and at most 50 milliseconds. Chrome 157 gave all of this.
    # The periods' ends are what is checked, not the time left: a busy runner can hold a callback up well into its
    # period, or start a period so late that the next timer cuts it short.
    shared = """
      new Promise((resolve) => {
        const read = (d) => [performance.now(), d.timeRemaining(), performance.now()];
        const out = {};
        requestIdleCallback((d) => {
          out.first = read(d);
          const start = performance.now();
          while (performance.now() - start < 5) {}
        });
        requestIdleCallback((d) => {
          out.second = read(d);
          requestIdleCallback((d2) => {
            out.next = read(d2);
            resolve(out);
          });
        });
      })
    """
    timers = """
      new Promise((resolve) => {
        const read = (d) => [performance.now(), d.timeRemaining(), performance.now()];
        const out = {};
        setTimeout(() => {
          const before = performance.now();
          setTimeout(() => {}, 20);
          out.nearTimer = [before + 20, performance.now() + 20];
          requestIdleCallback((d) => {
            out.near = read(d);
            setTimeout(() => {}, 200);
            requestIdleCallback((d2) => {
              out.far = read(d2);
              resolve(out);
            });
          });
        }, 0);
      })
    """

    out = _evaluate(program, shared)
    first, second, following = (_idle_period_end(out[key]) for key in ("first", "second", "next"))
    # The second shares the first's end, unless the first was held up past it, which starts a new period. The third
    # runs once that end has passed, in a period that lasts well beyond it.
    assert (first[0] <= second[1] and second[0] <= first[1]) or out["second"][0] + _CLAMPING_MS >= first[0], out
    assert out["next"][0] + _CLAMPING_MS >= second[0], out
    assert following[1] - second[0] > 30, out
    assert out["next"][1] <= 50, out

    out = _evaluate(program, timers)
    near, far = (_idle_period_end(out[key]) for key in ("near", "far"))
    soonest_due, latest_due = out["nearTimer"]
    # A period that starts before the timer is due ends by it, so one whose callback ran before then did. The next,
    # with no timer due for 200 milliseconds, lasts longer.
    assert out["near"][0] + _CLAMPING_MS >= soonest_due or near[0] <= latest_due + _CLAMPING_MS, out
    assert far[1] - near[0] > 20, out
    assert out["far"][1] <= 50, out


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


def test_scheduler_runs_tasks_by_priority_and_yield_inherits_as_chrome_does(program: Path) -> None:
    # What Chrome 157 gave once its page had loaded. A yield() continues before the tasks of its priority, which it
    # inherits from a postTask() callback and from an idle callback, whose priority is background, but not from a
    # timer.
    script = """
      new Promise(async (resolve) => {
        const out = {};
        const error = async (f) => { try { await f(); return 'none'; } catch (e) { return `${e?.name}: ${e?.message ?? e}`; } };
        out.shape = [typeof scheduler, Object.prototype.toString.call(scheduler), scheduler === window.scheduler,
          Object.getOwnPropertyNames(Scheduler.prototype).sort()];
        out.errors = {
          noArgs: await error(() => scheduler.postTask()),
          notFunction: await error(() => scheduler.postTask(5)),
          badPriority: await error(() => scheduler.postTask(() => {}, {priority: 'x'})),
          badDelay: await error(() => scheduler.postTask(() => {}, {delay: -1})),
          aborted: await error(() => scheduler.postTask(() => 'ran', {signal: AbortSignal.abort('why')})),
          construct: await error(() => new Scheduler()),
        };
        // Priority order: tasks posted in one task run by priority, FIFO within one.
        const order = [];
        const all = [
          scheduler.postTask(() => order.push('bg1'), {priority: 'background'}),
          scheduler.postTask(() => order.push('uv1')),
          scheduler.postTask(() => order.push('ub1'), {priority: 'user-blocking'}),
          scheduler.postTask(() => order.push('uv2'), {priority: 'user-visible'}),
          scheduler.postTask(() => order.push('ub2'), {priority: 'user-blocking'}),
          scheduler.postTask(() => order.push('bg2'), {priority: 'background'}),
        ];
        setTimeout(() => order.push('timeout0'), 0);
        queueMicrotask(() => order.push('micro'));
        await Promise.all(all);
        await new Promise(r => setTimeout(r, 5));
        out.order = order;
        // Values and errors.
        out.value = await scheduler.postTask(() => 42);
        out.thrown = await error(() => scheduler.postTask(() => { throw new RangeError('in task'); }));
        out.thisArgs = await scheduler.postTask(function () { return [this === undefined, this === window, arguments.length]; });
        // Abort before running, and while running async.
        const c1 = new AbortController();
        const p1 = scheduler.postTask(() => 'ran', {signal: c1.signal});
        c1.abort();
        out.abortedBefore = await error(() => p1);
        const c2 = new TaskController({priority: 'background'});
        const p2 = scheduler.postTask(async () => { await new Promise(r => setTimeout(r, 5)); return 'finished'; }, {signal: c2.signal});
        setTimeout(() => c2.abort(), 1);
        out.abortedAsync = await p2.then(v => v, e => `${e.name}`);
        // Delay.
        const t0 = performance.now();
        await scheduler.postTask(() => {}, {delay: 20});
        out.delayed = performance.now() - t0 >= 19;
        // yield: continuation before same-priority tasks; inheritance.
        const yorder = [];
        await scheduler.postTask(async () => {
          scheduler.postTask(() => yorder.push('uv task'));
          scheduler.postTask(() => yorder.push('bg task'), {priority: 'background'});
          await scheduler.yield();
          yorder.push('after yield (uv)');
        });
        await new Promise(r => setTimeout(r, 5));
        out.yieldOrder = yorder;
        const inherit = [];
        await scheduler.postTask(async () => {
          scheduler.postTask(() => inherit.push('uv task'));
          await scheduler.yield();
          inherit.push('bg continuation');
        }, {priority: 'background'});
        await new Promise(r => setTimeout(r, 5));
        out.inheritBackground = inherit;
        // Inheritance through a timer and through await of another promise.
        const viaTimer = [];
        await new Promise((done) => {
          scheduler.postTask(() => {
            setTimeout(async () => {
              scheduler.postTask(() => viaTimer.push('uv task'));
              await scheduler.yield();
              viaTimer.push('continuation');
              done();
            }, 0);
          }, {priority: 'background'});
        });
        await new Promise(r => setTimeout(r, 5));
        out.viaTimer = viaTimer;
        const viaIdle = [];
        await new Promise((done) => {
          scheduler.postTask(() => {
            requestIdleCallback(async () => {
              scheduler.postTask(() => viaIdle.push('uv task'));
              await scheduler.yield();
              viaIdle.push('continuation');
              done();
            });
          }, {priority: 'background'});
        });
        await new Promise(r => setTimeout(r, 5));
        out.viaIdle = viaIdle;
        // yield outside a task: user-visible continuation; vs a timeout.
        const top = [];
        setTimeout(() => top.push('timeout0'), 0);
        scheduler.postTask(() => top.push('uv task'));
        await scheduler.yield();
        top.push('continuation');
        await new Promise(r => setTimeout(r, 5));
        out.topLevelYield = top;
        // yield inherits the abort signal.
        const c3 = new AbortController();
        out.yieldAborted = await scheduler.postTask(async () => {
          c3.abort();
          try { await scheduler.yield(); return 'resumed'; } catch (e) { return e.name; }
        }, {signal: c3.signal}).then(v => v, e => 'task rejected ' + e.name);
        resolve(out);
      })
    """

    assert _evaluate(program, script) == {
        "shape": ["object", "[object Scheduler]", True, ["constructor", "postTask", "yield"]],
        "errors": {
            "noArgs": "TypeError: Failed to execute 'postTask' on 'Scheduler': 1 argument required, but only "
            "0 present.",
            "notFunction": "TypeError: Failed to execute 'postTask' on 'Scheduler': parameter 1 is not of "
            "type 'Function'.",
            "badPriority": "TypeError: Failed to execute 'postTask' on 'Scheduler': Failed to read the "
            "'priority' property from 'SchedulerPostTaskOptions': The provided value 'x' is "
            "not a valid enum value of type TaskPriority.",
            "badDelay": "TypeError: Failed to execute 'postTask' on 'Scheduler': Failed to read the 'delay' "
            "property from 'SchedulerPostTaskOptions': Value is outside the 'unsigned long long' "
            "value range.",
            "aborted": "undefined: why",
            "construct": "TypeError: Failed to construct 'Scheduler': Illegal constructor",
        },
        "order": ["micro", "ub1", "ub2", "uv1", "uv2", "timeout0", "bg1", "bg2"],
        "value": 42,
        "thrown": "RangeError: in task",
        "thisArgs": [False, True, 0],
        "abortedBefore": "AbortError: signal is aborted without reason",
        "abortedAsync": "finished",
        "delayed": True,
        "yieldOrder": ["after yield (uv)", "uv task", "bg task"],
        "inheritBackground": ["uv task", "bg continuation"],
        "viaTimer": ["continuation", "uv task"],
        "viaIdle": ["uv task", "continuation"],
        "topLevelYield": ["continuation", "timeout0", "uv task"],
        "yieldAborted": "task rejected AbortError",
    }


def test_a_timer_without_delay_runs_before_a_task_posted_after_it(program: Path) -> None:
    # Blink posts a timer of no delay as an immediate task, which keeps its place among the tasks posted with it.
    script = """
      new Promise((resolve) => {
        const order = [];
        setTimeout(() => order.push('timer'), 0);
        scheduler.postTask(() => order.push('user-visible task'));
        setTimeout(() => order.push('second timer'), 0);
        setTimeout(() => resolve(order), 10);
      })
    """

    assert _evaluate(program, script) == ["timer", "user-visible task", "second timer"]


def test_a_delayed_task_that_its_signal_aborted_keeps_the_command_line_waiting_no_longer(program: Path) -> None:
    # Blink cancels the queued task of an aborted postTask(), so nothing waits for its delay.
    script = """
      (() => {
        const controller = new AbortController();
        const task = scheduler.postTask(() => 'ran', {delay: 60000, signal: controller.signal});
        controller.abort();
        return task.catch((error) => error.name);
      })()
    """
    started = time.monotonic()

    assert _evaluate(program, script) == "AbortError"
    assert time.monotonic() - started < _TIMEOUT_SECONDS / 2


def test_fetch_fails_on_the_command_line_which_has_no_session(program: Path) -> None:
    script = "fetch('http://127.0.0.1:1/').then(() => 'fetched', error => String(error))"

    assert _evaluate(program, script) == "TypeError: Failed to fetch"
