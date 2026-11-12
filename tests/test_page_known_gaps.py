"""Tests of what a Known Gap does in the Page program: docs/runtime.md, "What a Known Gap does".

A member whose behaviour the Runtime lacks runs Chrome's checks and converts its arguments, then gives its type's
default. Where a case's outcome comes from Chrome's bindings, its expected value is what Chrome 157 gave for the same
script. These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as probe.js writes an outcome.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {script} }})();
            return "value: " + (typeof value === "object" && value !== null
                ? Object.prototype.toString.call(value) : String(value));
        }} catch (error) {{
            return "threw: " + error;
        }}
    }})()"""
    result = subprocess.run(
        [program, "-"], input=wrapped, capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


# What Chrome 157.0.8103.51 gave for each script, where the outcome comes from its bindings: the number of arguments,
# overload resolution, the conversion of each argument and its message, and the receiver.
_AS_CHROME = {
    "too few arguments": (
        "return new FormData().append('a')",
        "threw: TypeError: Failed to execute 'append' on 'FormData': 2 arguments required, but only 1 present.",
    ),
    "the overload that the number of arguments chooses": (
        "return new FormData().append('a', 'b', 'c', 'd')",
        "threw: TypeError: Failed to execute 'append' on 'FormData': parameter 2 is not of type 'Blob'.",
    ),
    "the overload that a type chooses": ("return new Path2D().roundRect(0, 0, 1, 1, 'x')", "value: undefined"),
    "an argument that does not convert": (
        "return new Path2D().roundRect(0, 0, 1, 1, [Symbol()])",
        "threw: TypeError: Failed to execute 'roundRect' on 'Path2D': Cannot convert a Symbol value to a number",
    ),
    "a constructor without new": (
        "return Highlight()",
        "threw: TypeError: Failed to construct 'Highlight': Please use the 'new' operator, this DOM object "
        "constructor cannot be called as a function.",
    ),
    "a constructor's argument of the wrong interface": (
        "return new Highlight({})",
        "threw: TypeError: Failed to construct 'Highlight': parameter 1 is not of type 'AbstractRange'.",
    ),
    "a dictionary that is not an object": (
        "return new IntersectionObserver(() => {}, 5)",
        "threw: TypeError: Failed to construct 'IntersectionObserver': The provided value is not of type "
        "'IntersectionObserverInit'.",
    ),
    "a dictionary's members read in order": (
        """const read = [];
        const init = {};
        for (const key of ['threshold', 'root', 'rootMargin', 'delay', 'trackVisibility']) {
            Object.defineProperty(init, key, {get() { read.push(key); return undefined; }});
        }
        new IntersectionObserver(() => {}, init);
        return read.join();""",
        "value: delay,root,rootMargin,threshold,trackVisibility",
    ),
    "a callback that is not a function": (
        "return new IntersectionObserver(5)",
        "threw: TypeError: Failed to construct 'IntersectionObserver': parameter 1 is not of type 'Function'.",
    ),
    "a setter's integer, modulo its type's values": (
        "const h = new Highlight(); h.priority = 2 ** 31 + 5; return h.priority;",
        "value: -2147483643",
    ),
    "a setter ignores a string that is not of its enumeration": (
        "const h = new Highlight(); h.type = 'bogus'; return h.type;",
        "value: highlight",
    ),
    "a setter called with no argument": (
        "return Object.getOwnPropertyDescriptor(Highlight.prototype, 'priority').set.call(new Highlight())",
        "value: undefined",
    ),
    "a promise's wrong receiver": (
        "return Permissions.prototype.query.call({}, {name: 'geolocation'})",
        "threw: TypeError: Failed to execute 'query' on 'Permissions': Illegal invocation",
    ),
    "a promise's missing argument": (
        "return navigator.permissions.query()",
        "threw: TypeError: Failed to execute 'query' on 'Permissions': 1 argument required, but only 0 present.",
    ),
    "an object of EventTarget's behind an interface without a class": (
        """const signal = new AbortController().signal;
        let calls = 0;
        signal.addEventListener('x', () => calls++);
        signal.dispatchEvent(new Event('x'));
        return calls;""",
        "value: 1",
    ),
    "an object of Event's behind an interface without a class": (
        "const e = new CustomEvent('ping', {bubbles: true}); return [e.type, e.bubbles, e.cancelable].join();",
        "value: ping,true,false",
    ),
    "an event handler keeps an object, and null for anything else": (
        """const signal = new AbortController().signal;
        const handler = () => {};
        signal.onabort = handler;
        const kept = signal.onabort === handler;
        signal.onabort = 5;
        return [kept, signal.onabort].join();""",
        "value: true,",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_AS_CHROME.values()), ids=list(_AS_CHROME))
def test_a_known_gap_checks_and_converts_as_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome


@pytest.mark.parametrize(
    ("script", "expected"),
    [
        pytest.param("return window.name === ''", "value: true", id="string"),
        pytest.param("return new Highlight().priority", "value: 0", id="integer"),
        pytest.param("return new AbortController().signal.aborted", "value: false", id="boolean"),
        pytest.param("return new AbortController().signal.reason", "value: undefined", id="any"),
        pytest.param("return new Highlight().type", "value: highlight", id="first value of an enumeration"),
        pytest.param("return document.body.childNodes instanceof NodeList", "value: true", id="interface"),
        pytest.param(
            "return document.implementation === document.implementation", "value: true", id="the same object each time"
        ),
        pytest.param(
            "const c = new AbortController(); return c.signal === c.signal && c.signal instanceof AbortSignal",
            "value: true",
            id="the same object on one receiver",
        ),
        pytest.param(
            "return new AbortController().signal !== new AbortController().signal",
            "value: true",
            id="another object on another receiver",
        ),
        pytest.param("return navigator.storage.estimate()", "value: [object Object]", id="promise of a dictionary"),
        pytest.param(
            "const status = await navigator.permissions.query({name: 'geolocation'}); "
            "return status instanceof PermissionStatus",
            "value: true",
            id="promise of an interface",
        ),
    ],
)
def test_a_known_gap_gives_its_types_default(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


def test_a_setter_stores_the_converted_value_for_its_getter(program: Path) -> None:
    script = "const h = new Highlight(); h.priority = '7'; const first = h.priority; h.type = 'spelling-error'; " + (
        "return [first, typeof first, h.type].join();"
    )

    assert _evaluate(program, script) == "value: 7,number,spelling-error"


def test_a_constructor_gives_an_object_of_its_interface(program: Path) -> None:
    script = "const h = new Highlight(); return String([h instanceof Highlight, Object.getPrototypeOf(h) === Highlight.prototype]);"

    assert _evaluate(program, script) == "value: true,true"


@pytest.mark.parametrize(
    ("script", "message"),
    [
        pytest.param("return crypto.randomUUID()", "Failed to execute 'randomUUID' on 'Crypto'", id="crypto"),
        pytest.param("return document.createDocumentFragment()", "Failed to execute", id="an object of a class"),
    ],
)
def test_a_refused_known_gap_keeps_todays_error(program: Path, script: str, message: str) -> None:
    # Its default would be predictable random values, or an object of a class with nothing behind it.
    assert _evaluate(program, script).startswith(f"threw: TypeError: {message}")
    assert _evaluate(program, script).endswith("Not implemented.")


# Event handler attributes are not Known Gaps: each is a listener of its EventTarget, as Chrome 157 showed.
_HANDLERS_AS_CHROME = {
    "a handler keeps its place, and false cancels": (
        """const seen = [];
        const signal = new AbortController().signal;
        signal.addEventListener('abort', () => seen.push('first'));
        signal.onabort = () => seen.push('handler');
        signal.addEventListener('abort', () => seen.push('last'));
        signal.onabort = function () { seen.push('replaced ' + (this === signal)); return false; };
        const event = new Event('abort', {cancelable: true});
        seen.push(String(signal.dispatchEvent(event)), String(event.defaultPrevented));
        signal.onabort = null;
        signal.dispatchEvent(new Event('abort'));
        return seen.join('|');""",
        "value: first|replaced true|last|false|true|first|last",
    ),
    "a handler is not the listener of the same function": (
        """const signal = new AbortController().signal;
        const f = () => {};
        signal.onabort = f;
        signal.addEventListener('abort', f);
        let calls = 0;
        signal.onabort = () => calls++;
        signal.removeEventListener('abort', f);
        signal.dispatchEvent(new Event('abort'));
        return calls;""",
        "value: 1",
    ),
    "the body's window events are the window's": (
        "document.body.onload = () => 1; return window.onload === document.body.onload;",
        "value: true",
    ),
    "a handler that cannot be called does nothing": (
        "const signal = new AbortController().signal; signal.onabort = {}; "
        "return signal.dispatchEvent(new Event('abort'));",
        "value: true",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_HANDLERS_AS_CHROME.values()), ids=list(_HANDLERS_AS_CHROME))
def test_an_event_handler_attribute_is_a_listener_as_in_chrome(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome
