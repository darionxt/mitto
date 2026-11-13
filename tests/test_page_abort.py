"""Tests of AbortController and AbortSignal in the Page program: docs/runtime.md, "Abort signals".

Both have behaviour, Blink's, in native/dom: abort(), the reasons and their DOMExceptions, `abort` events,
AbortSignal.abort(), timeout() and any(), throwIfAborted(), addEventListener()'s `signal`, TaskController's TaskSignal
and Request's signal. Each expected value is what Chrome 157 gave for the same script. What a signal does to a fetch()
through a Session is in tests/test_page_loading.py. These run where MITTO_PAGE_PROGRAM names the Page program, as
tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

# A helper that the scripts share: what each of `signals` sees fire, as "name:type", into `log`.
_PRELUDE = """
const log = [];
const watch = (signals) => {
    for (const [name, signal] of Object.entries(signals)) {
        signal.addEventListener("abort", (event) => log.push(name + ":" + event.type));
    }
};
"""


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str, *flags: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_known_gaps.py writes it.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {_PRELUDE} {script} }})();
            return "value: " + (typeof value === "object" && value !== null
                ? Object.prototype.toString.call(value) : String(value));
        }} catch (error) {{
            return "threw: " + error;
        }}
    }})()"""
    result = subprocess.run(
        [program, *flags, "-"],
        input=wrapped,
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


# What Chrome 157.0.8103.51 gave for each script.
_AS_CHROME = {
    "a new controller's signal": (
        "const s = new AbortController().signal;"
        " return [Object.prototype.toString.call(s), s.aborted, s.reason, s.onabort].join()",
        "value: [object AbortSignal],false,,",
    ),
    "the signal is the same object each time": (
        "const c = new AbortController(); return c.signal === c.signal",
        "value: true",
    ),
    "abort() with no reason": (
        "const c = new AbortController(); watch({c: c.signal}); c.abort(); const r = c.signal.reason;"
        " return [c.signal.aborted, r instanceof DOMException, r.name, r.message, r.code, typeof r.stack, log].join()",
        "value: true,true,AbortError,signal is aborted without reason,20,string,c:abort",
    ),
    "abort() with undefined": (
        "const c = new AbortController(); c.abort(undefined); return c.signal.reason.message",
        "value: signal is aborted without reason",
    ),
    "abort() with a reason": (
        "const c = new AbortController(); const reason = {}; c.abort(reason); return c.signal.reason === reason",
        "value: true",
    ),
    "abort() with null": (
        "const c = new AbortController(); c.abort(null); return c.signal.reason",
        "value: null",
    ),
    "a second abort() keeps the first reason and fires nothing": (
        "const c = new AbortController(); watch({c: c.signal}); c.abort('first'); c.abort('second');"
        " return [c.signal.reason, log].join()",
        "value: first,c:abort",
    ),
    "the abort event": (
        "const c = new AbortController(); let seen; c.signal.onabort = function (event) {"
        " seen = [this === c.signal, event.type, event.isTrusted, event.bubbles, event.cancelable, event.composed,"
        " event.target === c.signal, event.eventPhase, Object.prototype.toString.call(event)]; };"
        " c.abort(); return seen.join()",
        "value: true,abort,true,false,false,false,true,2,[object Event]",
    ),
    "listeners and the handler run in the order they were added": (
        "const c = new AbortController(); const s = c.signal;"
        " s.addEventListener('abort', () => log.push('first')); s.onabort = () => log.push('handler');"
        " s.addEventListener('abort', () => log.push('last')); c.abort(); return log.join()",
        "value: first,handler,last",
    ),
    "the reason is set before listeners run": (
        "const c = new AbortController(); let seen;"
        " c.signal.addEventListener('abort', () => { seen = [c.signal.aborted, c.signal.reason]; }); c.abort('r');"
        " return seen.join()",
        "value: true,r",
    ),
    "throwIfAborted() before abort": (
        "return new AbortController().signal.throwIfAborted()",
        "value: undefined",
    ),
    "throwIfAborted() throws the reason as it is": (
        "const error = new Error('mine'); const s = AbortSignal.abort(error);"
        " try { s.throwIfAborted(); } catch (thrown) { return [thrown === error, thrown.message].join(); }",
        "value: true,mine",
    ),
    "throwIfAborted() throws a reason that is not an object": (
        "AbortSignal.abort('just a string').throwIfAborted()",
        "threw: just a string",
    ),
    "throwIfAborted() throws the default reason": (
        "AbortSignal.abort().throwIfAborted()",
        "threw: AbortError: signal is aborted without reason",
    ),
    "throwIfAborted() on another object": (
        "AbortSignal.prototype.throwIfAborted.call({})",
        "threw: TypeError: Illegal invocation",
    ),
    "AbortSignal.abort()": (
        "const s = AbortSignal.abort(); const t = AbortSignal.abort('r');"
        " return [s.aborted, s.reason.name, s.reason.message, t.reason, s !== AbortSignal.abort()].join()",
        "value: true,AbortError,signal is aborted without reason,r,true",
    ),
    "AbortSignal.timeout() aborts in a later task": (
        "const s = AbortSignal.timeout(1); watch({s}); const before = s.aborted;"
        " await new Promise((resolve) => setTimeout(resolve, 50)); const r = s.reason;"
        " return [before, s.aborted, r.name, r.message, r.code, log].join()",
        "value: false,true,TimeoutError,signal timed out,23,s:abort",
    ),
    "AbortSignal.timeout() of 0 runs after a timer set before it": (
        "setTimeout(() => log.push('timer')); const s = AbortSignal.timeout(0); watch({s});"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return log.join()",
        "value: timer,s:abort",
    ),
    "a page waits for AbortSignal.timeout() alone": (
        "return await new Promise((resolve) => { AbortSignal.timeout(10).onabort = (event) => resolve(event.type); })",
        "value: abort",
    ),
    "AbortSignal.timeout() without an argument": (
        "return AbortSignal.timeout()",
        "threw: TypeError: Failed to execute 'timeout' on 'AbortSignal': 1 argument required, but only 0 present.",
    ),
    "AbortSignal.timeout() of a negative number": (
        "return AbortSignal.timeout(-1)",
        (
            "threw: TypeError: Failed to execute 'timeout' on 'AbortSignal': Value is outside the 'unsigned long long' "
            "value range."
        ),
    ),
    "AbortSignal.timeout() of NaN": (
        "return AbortSignal.timeout(NaN)",
        "threw: TypeError: Failed to execute 'timeout' on 'AbortSignal': Value is not of type 'unsigned long long'.",
    ),
    "AbortSignal.timeout() of Infinity": (
        "return AbortSignal.timeout(Infinity)",
        (
            "threw: TypeError: Failed to execute 'timeout' on 'AbortSignal': Value is infinite and not of type "
            "'unsigned long long'."
        ),
    ),
    "AbortSignal.timeout() beyond 2^53": (
        "return AbortSignal.timeout(2 ** 53)",
        (
            "threw: TypeError: Failed to execute 'timeout' on 'AbortSignal': Value is outside the 'unsigned long long' "
            "value range."
        ),
    ),
    # A larger timeout would keep the program, which waits for every timer, running past the test's limit.
    "AbortSignal.timeout() truncates": (
        "const s = AbortSignal.timeout(-0.9); const t = AbortSignal.timeout('7');"
        " const before = [s.aborted, t.aborted];"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return [...before, s.aborted, t.aborted].join()",
        "value: false,false,true,true",
    ),
    "AbortSignal.any() of nothing never aborts": (
        "const s = AbortSignal.any([]); return [s.aborted, s.reason, s instanceof AbortSignal].join()",
        "value: false,,true",
    ),
    "AbortSignal.any() of an aborted signal": (
        "const reason = {}; const s = AbortSignal.any([new AbortController().signal, AbortSignal.abort(reason),"
        " AbortSignal.abort('later')]); return [s.aborted, s.reason === reason].join()",
        "value: true,true",
    ),
    "AbortSignal.any() follows each signal": (
        "const a = new AbortController(); const b = new AbortController();"
        " const s = AbortSignal.any([a.signal, b.signal]);"
        " watch({a: a.signal, b: b.signal, s}); b.abort('b'); a.abort('a'); return [s.reason, log].join()",
        "value: b,b:abort,s:abort,a:abort",
    ),
    "a composite signal follows another's sources": (
        "const c = new AbortController(); const a = AbortSignal.any([c.signal]); const b = AbortSignal.any([a]);"
        " const d = AbortSignal.any([b, c.signal]); let seen; c.signal.addEventListener('abort', () => {"
        " seen = [a.aborted, b.aborted, d.aborted, d.reason === c.signal.reason]; });"
        " watch({c: c.signal, a, b, d}); c.abort(); return [seen, log].join()",
        "value: true,true,true,true,c:abort,a:abort,b:abort,d:abort",
    ),
    "a composite signal that a timeout aborts": (
        "const s = AbortSignal.any([new AbortController().signal, AbortSignal.timeout(1)]);"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return [s.aborted, s.reason.name].join()",
        "value: true,TimeoutError",
    ),
    "a composite signal kept by its listener": (
        "const c = new AbortController();"
        " AbortSignal.any([c.signal]).addEventListener('abort', () => log.push('fired'));"
        " for (let i = 0; i < 3; i++) { await new Promise((resolve) => setTimeout(resolve)); }"
        " c.abort(); return log.join()",
        "value: fired",
    ),
    "AbortSignal.any() of the same signal twice": (
        "const c = new AbortController(); const s = AbortSignal.any([c.signal, c.signal]); watch({s}); c.abort();"
        " return log.join()",
        "value: s:abort",
    ),
    "AbortSignal.any() without an argument": (
        "return AbortSignal.any()",
        "threw: TypeError: Failed to execute 'any' on 'AbortSignal': 1 argument required, but only 0 present.",
    ),
    "AbortSignal.any() of something else": (
        "return AbortSignal.any([{}])",
        "threw: TypeError: Failed to execute 'any' on 'AbortSignal': Failed to convert value to 'AbortSignal'.",
    ),
    "AbortSignal.any() of what is not a sequence": (
        "return AbortSignal.any(5)",
        (
            "threw: TypeError: Failed to execute 'any' on 'AbortSignal': The provided value cannot be converted to a "
            "sequence."
        ),
    ),
    "AbortSignal has no constructor": (
        "return new AbortSignal()",
        "threw: TypeError: Failed to construct 'AbortSignal': Illegal constructor",
    ),
    "addEventListener() with a signal": (
        "const c = new AbortController(); const t = new EventTarget(); t.addEventListener('x', () => log.push('x'),"
        " {signal: c.signal}); t.dispatchEvent(new Event('x')); c.abort(); t.dispatchEvent(new Event('x'));"
        " return log.join()",
        "value: x",
    ),
    "addEventListener() with an aborted signal adds nothing": (
        "const t = new EventTarget(); t.addEventListener('x', () => log.push('x'), {signal: AbortSignal.abort()});"
        " t.dispatchEvent(new Event('x')); return log.length",
        "value: 0",
    ),
    "a listener of a signal's own abort that the signal removes": (
        "const c = new AbortController(); c.signal.addEventListener('abort', () => log.push('removed first'),"
        " {signal: c.signal}); c.signal.addEventListener('abort', () => log.push('stays')); c.abort();"
        " return log.join()",
        "value: stays",
    ),
    "the signal removes the listener with the same capture": (
        "const c = new AbortController(); const t = new EventTarget(); const f = () => log.push('f');"
        " t.addEventListener('x', f, {signal: c.signal}); t.addEventListener('x', f, {capture: true});"
        " c.abort(); t.dispatchEvent(new Event('x')); return log.join()",
        "value: f",
    ),
    "the signal removes a listener added again without it": (
        "const c = new AbortController(); const t = new EventTarget(); const f = () => log.push('f');"
        " t.addEventListener('x', f, {signal: c.signal}); t.removeEventListener('x', f); t.addEventListener('x', f);"
        " c.abort(); t.dispatchEvent(new Event('x')); return log.length",
        "value: 0",
    ),
    "a once listener with a signal": (
        "const c = new AbortController(); const t = new EventTarget();"
        " t.addEventListener('x', () => log.push('x'), {once: true, signal: c.signal});"
        " t.dispatchEvent(new Event('x')); t.dispatchEvent(new Event('x')); c.abort(); return log.join()",
        "value: x",
    ),
    "a listener's signal from AbortSignal.any()": (
        "const c = new AbortController(); const t = new EventTarget();"
        " t.addEventListener('x', () => log.push('x'), {signal: AbortSignal.any([c.signal])});"
        " for (let i = 0; i < 3; i++) { await new Promise((resolve) => setTimeout(resolve)); }"
        " c.abort(); t.dispatchEvent(new Event('x')); return log.length",
        "value: 0",
    ),
    "addEventListener() with a signal of null": (
        "new EventTarget().addEventListener('x', () => {}, {signal: null})",
        (
            "threw: TypeError: Failed to execute 'addEventListener' on 'EventTarget': Failed to read the 'signal' "
            "property from 'AddEventListenerOptions': Failed to convert value to 'AbortSignal'."
        ),
    ),
    "addEventListener() with a signal that is not one": (
        "new EventTarget().addEventListener('x', () => {}, {signal: {}})",
        (
            "threw: TypeError: Failed to execute 'addEventListener' on 'EventTarget': Failed to read the 'signal' "
            "property from 'AddEventListenerOptions': Failed to convert value to 'AbortSignal'."
        ),
    ),
    "addEventListener() with an undefined signal": (
        "const t = new EventTarget(); t.addEventListener('x', () => log.push('x'), {signal: undefined});"
        " t.dispatchEvent(new Event('x')); return log.join()",
        "value: x",
    ),
    "TaskController": (
        "const c = new TaskController(); const s = c.signal;"
        " return [Object.prototype.toString.call(c), Object.prototype.toString.call(s), s instanceof AbortSignal,"
        " s.priority, s.aborted, c.signal === s].join()",
        "value: [object TaskController],[object TaskSignal],true,user-visible,false,true",
    ),
    "TaskController with a priority": (
        "return new TaskController({priority: 'background'}).signal.priority",
        "value: background",
    ),
    "TaskController with a priority that is not one": (
        "return new TaskController({priority: 'urgent'})",
        (
            "threw: TypeError: Failed to construct 'TaskController': Failed to read the 'priority' property from "
            "'TaskControllerInit': The provided value 'urgent' is not a valid enum value of type TaskPriority."
        ),
    ),
    "TaskController aborts its TaskSignal": (
        "const c = new TaskController(); watch({s: c.signal}); c.abort('r'); return [c.signal.reason, log].join()",
        "value: r,s:abort",
    ),
    "TaskSignal.any()": (
        "const s = TaskSignal.any([]); return [Object.prototype.toString.call(s), s.priority, s.aborted].join()",
        "value: [object TaskSignal],user-visible,false",
    ),
    "a Request's signal": (
        "const r = new Request('https://a.test/');"
        " return [Object.prototype.toString.call(r.signal), r.signal.aborted, r.signal === r.signal].join()",
        "value: [object AbortSignal],false,true",
    ),
    "a Request's signal follows the one it is given": (
        "const c = new AbortController(); const r = new Request('https://a.test/', {signal: c.signal});"
        " watch({c: c.signal, r: r.signal}); const before = r.signal.aborted; c.abort();"
        " return [r.signal !== c.signal, before, r.signal.aborted, r.signal.reason === c.signal.reason, log].join()",
        "value: true,false,true,true,c:abort,r:abort",
    ),
    "a Request made from another follows its signal": (
        "const c = new AbortController(); const r = new Request(new Request('https://a.test/', {signal: c.signal}));"
        " c.abort('r'); return r.signal.reason",
        "value: r",
    ),
    "a Request with a signal of null follows nothing": (
        "const c = new AbortController();"
        " const r = new Request(new Request('https://a.test/', {signal: c.signal}), {signal: null});"
        " c.abort(); return r.signal.aborted",
        "value: false",
    ),
    "a Request's signal that is not one": (
        "return new Request('https://a.test/', {signal: {}})",
        (
            "threw: TypeError: Failed to construct 'Request': Failed to read the 'signal' property from 'RequestInit': "
            "Failed to convert value to 'AbortSignal'."
        ),
    ),
    "fetch() with an aborted signal rejects with its reason": (
        "const reason = new Error('gone');"
        " try { await fetch('https://a.test/', {signal: AbortSignal.abort(reason)}); }"
        " catch (error) { return error === reason; }",
        "value: true",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_AS_CHROME.values()), ids=list(_AS_CHROME))
def test_an_abort_signal_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome


# What a collection between the steps must not change: each gives what Chrome gave for the same script with no
# collection, in the _AS_CHROME cases of the same names. gc() is V8's, which --expose-gc gives the Page program.
_AFTER_A_COLLECTION = {
    "a composite signal kept by its listener": (
        "const c = new AbortController();"
        " AbortSignal.any([c.signal]).addEventListener('abort', () => log.push('fired'));"
        " gc(); c.abort(); return log.join()",
        "value: fired",
    ),
    "a listener's signal from AbortSignal.any()": (
        "const c = new AbortController(); const t = new EventTarget();"
        " t.addEventListener('x', () => log.push('x'), {signal: AbortSignal.any([c.signal])});"
        " gc(); c.abort(); t.dispatchEvent(new Event('x')); return log.length",
        "value: 0",
    ),
    "the signal removes a listener added again without it": (
        "const c = new AbortController(); const t = new EventTarget(); const f = () => log.push('f');"
        " t.addEventListener('x', f, {signal: c.signal}); t.removeEventListener('x', f); gc();"
        " t.addEventListener('x', f); gc(); c.abort(); t.dispatchEvent(new Event('x')); return log.length",
        "value: 0",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_AFTER_A_COLLECTION.values()), ids=list(_AFTER_A_COLLECTION))
def test_a_collection_changes_nothing_a_signal_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script, "--js-flags=--expose-gc") == chrome


def test_a_signal_keeps_no_listener_alive_that_the_page_removed(program: Path) -> None:
    # Blink's abort algorithm holds the listener weakly, so a function that the page removed and let go is collected
    # while the signal lives on, as here.
    script = """
        const c = new AbortController();
        const t = new EventTarget();
        let ref;
        (() => {
            const f = () => {};
            ref = new WeakRef(f);
            t.addEventListener('x', f, {signal: c.signal});
            t.removeEventListener('x', f);
        })();
        await new Promise((resolve) => setTimeout(resolve));
        gc();
        return [ref.deref() === undefined, c.signal.aborted].join();
    """

    assert _evaluate(program, script, "--js-flags=--expose-gc") == "value: true,false"
