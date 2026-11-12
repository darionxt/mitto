"""Tests of the bindings in the Page program, which the generator in native/bindings writes from Blink's Web IDL files.

These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_program.py does. What a page sees of every
interface, the global object's own properties among it, is the Parity Checks' of the Script Fingerprint
(docs/parity.md). Here, tests/page_cases.js runs in the Page program, and what some calls throw or do, and the own
properties of an instance of each class that the Runtime implements, are compared with what Chrome 157.0.8103.39 gave for
the same script, which tests/data/page_cases.json keeps.
"""

import json
import os
import subprocess
from pathlib import Path
from typing import Any

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_WRONG_ARGUMENTS = 2
_TESTS = Path(__file__).resolve().parent
_CASES = _TESTS / "page_cases.js"
_AS_CHROME = _TESTS / "data" / "page_cases.json"


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, *arguments: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *arguments],
        input=stdin,
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )


def _evaluate(program: Path, script: str, *options: str) -> str:
    result = _run(program, *options, script)
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


_CHROME: dict[str, Any] = json.loads(_AS_CHROME.read_text(encoding="utf-8"))
"""What Chrome 157.0.8103.39 gave for tests/page_cases.js, which M6 recorded on Windows."""


@pytest.fixture(scope="module")
def page(program: Path) -> dict[str, Any]:
    """What the Page program gives for the same script, which it reads from standard input: the script is longer than a
    command line holds on Windows."""
    result = _run(program, "-", stdin=_CASES.read_text(encoding="utf-8"))
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


@pytest.mark.parametrize("name", list(_CHROME["instances"]))
def test_an_instance_has_chromes_own_properties(page: dict[str, Any], name: str) -> None:
    # Such as Event's isTrusted and Document's location, which are [LegacyUnforgeable], and none for the others.
    assert page["instances"][name] == _CHROME["instances"][name]


def test_the_cases_go_as_in_chrome(page: dict[str, Any]) -> None:
    assert [name for name, _ in page["cases"]] == [name for name, _ in _CHROME["cases"]]
    for (name, outcome), (_, expected) in zip(page["cases"], _CHROME["cases"], strict=True):
        assert (name, outcome) == (name, expected)


def test_new_event_and_dispatch_work(program: Path) -> None:
    script = """
        const target = new EventTarget();
        const seen = [];
        target.addEventListener("ping", (event) => seen.push(event.type, event.target === target));
        [target.dispatchEvent(new Event("ping")), ...seen].join()
    """

    assert _evaluate(program, script) == "true,ping,true"


def test_listeners_outlive_garbage_collection(program: Path) -> None:
    # Only the target's wrapper keeps its listeners, through V8's C++ heap, while scripts allocate enough to collect.
    script = """
        const targets = [];
        for (let i = 0; i < 2000; i++) {
          const target = new EventTarget();
          target.addEventListener("x", () => { target.count = (target.count || 0) + 1; });
          targets.push(target);
        }
        for (let i = 0; i < 200000; i++) new Array(64).fill(i);
        for (const target of targets) target.dispatchEvent(new Event("x"));
        targets.every((target) => target.count === 1)
    """

    assert _evaluate(program, script) == "true"


def test_a_page_without_a_feature_lacks_the_members_behind_it(program: Path) -> None:
    # PopStateEvent's hasUAVisualTransition is behind HasUAVisualTransition, which is on for stable.
    script = "['hasUAVisualTransition' in PopStateEvent.prototype, Object.getOwnPropertyNames(PopStateEvent.prototype)]"

    assert _evaluate(program, script) == "true,state,hasUAVisualTransition,constructor"
    assert _evaluate(program, script, "--disable-blink-features=HasUAVisualTransition") == "false,state,constructor"


def test_a_feature_can_be_turned_on_and_off_as_in_chrome(program: Path) -> None:
    script = "'hasUAVisualTransition' in PopStateEvent.prototype"
    options = ["--disable-blink-features=HasUAVisualTransition", "--enable-blink-features=HasUAVisualTransition"]

    # As in Chrome, a feature that both name ends up off.
    assert _evaluate(program, script, *options) == "false"
    assert _evaluate(program, script, "--enable-blink-features=HasUAVisualTransition") == "true"


@pytest.mark.parametrize(
    ("disabled", "lengths"),
    [
        pytest.param([], "1,0", id="stable"),
        # Without it the current target stays after dispatch, and the path still ends with the dispatch.
        pytest.param(["ClearCurrentTargetAfterDispatch"], "1,0", id="current-target-kept"),
        # Without it Blink gives a path only to a window, which no target of the Runtime is.
        pytest.param(["ComposedPathReturnTargetBeingDispatched"], "0,0", id="no-path"),
    ],
)
def test_the_composed_path_follows_the_features_as_in_blink(program: Path, disabled: list[str], lengths: str) -> None:
    script = """
        const target = new EventTarget();
        const event = new Event("x");
        let during;
        target.addEventListener("x", () => { during = event.composedPath().length; });
        target.dispatchEvent(event);
        [during, event.composedPath().length].join()
    """
    options = [f"--disable-blink-features={','.join(disabled)}"] if disabled else []

    assert _evaluate(program, script, *options) == lengths


def test_it_refuses_a_feature_it_does_not_know(program: Path) -> None:
    result = _run(program, "--disable-blink-features=HasUAVisualTransition,NoSuchFeature", "1")

    assert (result.returncode, result.stdout) == (_WRONG_ARGUMENTS, "")
    assert result.stderr == "mitto_page: no runtime-enabled feature is named NoSuchFeature\n"


# Objects whose class has an indexed and a named property getter, Web IDL's legacy platform objects, whose own
# properties V8 asks interceptors for: navigator.plugins and navigator.mimeTypes, and a plugin. Each case is what
# Chrome of the Engine Release gave for the same expression (Mac Chrome 157.0.8103.51, headless), what it gives or
# what it throws, with the cases run in this order, since a few of them add properties.
_LEGACY_PLATFORM_OBJECTS_PRELUDE = """
const describe = (object) => Reflect.ownKeys(object).map((key) => {
  const d = Object.getOwnPropertyDescriptor(object, key);
  const value = d.get ? "a" : typeof d.value === "function" ? "f"
      : typeof d.value === "object" && d.value !== null ? Object.prototype.toString.call(d.value) : JSON.stringify(d.value);
  return String(key) + ":" + value + (d.enumerable ? "e" : "") + (d.configurable ? "c" : "") + (d.writable ? "w" : "");
}).join(" ");
const descriptor = (object, key) =>
    JSON.stringify(Object.getOwnPropertyDescriptor(object, key), (k, v) => (k === "value" ? String(v) : v));
const sloppySet = new Function("o", "k", "v", "o[k] = v; return o[k];");
const sloppyDelete = new Function("o", "k", "return delete o[k];");
const P = navigator.plugins;
const M = navigator.mimeTypes;
"""

_LEGACY_PLATFORM_OBJECTS_AS_CHROME = {
    "own properties of navigator.plugins": (
        "describe(P)",
        "gives 0:[object Plugin]ec 1:[object Plugin]ec 2:[object Plugin]ec 3:[object Plugin]ec 4:[object Plugin]ec PDF Viewer:[object Plugin]c Chrome PDF Viewer:[object Plugin]c Chromium PDF Viewer:[object Plugin]c Microsoft Edge PDF Viewer:[object Plugin]c WebKit built-in PDF:[object Plugin]c",
    ),
    "own properties of navigator.mimeTypes": (
        "describe(M)",
        "gives 0:[object MimeType]ec 1:[object MimeType]ec application/pdf:[object MimeType]c text/pdf:[object MimeType]c",
    ),
    "own properties of a plugin": (
        "describe(P[0])",
        "gives 0:[object MimeType]ec 1:[object MimeType]ec application/pdf:[object MimeType]c text/pdf:[object MimeType]c",
    ),
    "own properties of a mime type": (
        "describe(M[0])",
        "gives ",
    ),
    "for-in over navigator.plugins": (
        "(() => { const keys = []; for (const key in P) keys.push(key); return keys.join(); })()",
        "gives 0,1,2,3,4,length,item,namedItem,refresh",
    ),
    "for-in over a plugin": (
        "(() => { const keys = []; for (const key in P[0]) keys.push(key); return keys.join(); })()",
        "gives 0,1,name,filename,description,length,item,namedItem",
    ),
    "Object.keys": (
        "Object.keys(P).join()",
        "gives 0,1,2,3,4",
    ),
    "spread of navigator.plugins": (
        "[...P].map((p) => p.name).join('|')",
        "gives PDF Viewer|Chrome PDF Viewer|Chromium PDF Viewer|Microsoft Edge PDF Viewer|WebKit built-in PDF",
    ),
    "spread of navigator.mimeTypes": (
        "[...M].map((m) => m.type).join('|')",
        "gives application/pdf|text/pdf",
    ),
    "spread of a plugin": (
        "[...P[1]].map((m) => m.type + '/' + m.enabledPlugin.name).join('|')",
        "gives application/pdf/Chrome PDF Viewer|text/pdf/Chrome PDF Viewer",
    ),
    "Array.from of a plugin": (
        "Array.from(P[0]).length",
        "gives 2",
    ),
    "values": (
        "[P.length, P[0].name, P[0].filename, P[0].description, P[0].length, M.length, M[0].type, M[0].suffixes, M[0].description, M[1].type].join('|')",
        "gives 5|PDF Viewer|internal-pdf-viewer|Portable Document Format|2|2|application/pdf|pdf|Portable Document Format|text/pdf",
    ),
    "the same object": (
        "[P === navigator.plugins, M === navigator.mimeTypes, P[0] === P[0], P[0] === P.item(0), P['PDF Viewer'] === P[0], P.namedItem('Chrome PDF Viewer') === P[1], M[0] === M[0], M[0] === M['application/pdf'], M[0].enabledPlugin === P[0], P[2][1].enabledPlugin === P[2]].join()",
        "gives true,true,true,true,true,true,true,true,true,true",
    ),
    "a new mime type each time": (
        "[P[0][0] === P[0][0], P[0][0] === M[0], P[0].item(0) === P[0].item(0)].join()",
        "gives false,false,false",
    ),
    "what is not there": (
        "[P[5], P.item(5), P.item(-1), P.namedItem('x'), P['x'], M[2], P[0][2], P[0]['image/png']].map(String).join()",
        "gives undefined,null,null,null,undefined,undefined,undefined,undefined",
    ),
    "an index converted to unsigned long": (
        "P.item(2 ** 32).name",
        "gives PDF Viewer",
    ),
    "names that are not indices": (
        "[P['00'], P['1.0'], P[' 1'], P['-0']].map(String).join()",
        "gives undefined,undefined,undefined,undefined",
    ),
    "in": (
        "['0' in P, '5' in P, 'PDF Viewer' in P, 'x' in P, 'application/pdf' in M, 'text/pdf' in P[0], 'length' in P].join()",
        "gives true,false,true,false,true,true,true",
    ),
    "hasOwnProperty": (
        "['0', 'PDF Viewer', 'length', 'item'].map((key) => Object.prototype.hasOwnProperty.call(P, key)).join()",
        "gives true,true,false,false",
    ),
    "descriptor of an index": (
        "descriptor(P, '0')",
        'gives {"value":"[object Plugin]","writable":false,"enumerable":true,"configurable":true}',
    ),
    "descriptor of a name": (
        "descriptor(P, 'PDF Viewer')",
        'gives {"value":"[object Plugin]","writable":false,"enumerable":false,"configurable":true}',
    ),
    "descriptor of a mime type's name": (
        "descriptor(M, 'text/pdf')",
        'gives {"value":"[object MimeType]","writable":false,"enumerable":false,"configurable":true}',
    ),
    "an index set in strict code": (
        "(() => { P[0] = 1; return P[0]; })()",
        "throws TypeError: Failed to set an indexed property [0] on 'PluginArray': Indexed property setter is not supported.",
    ),
    "an index set in sloppy code": (
        "String(sloppySet(P, 0, 1))",
        "gives [object Plugin]",
    ),
    "an index out of range set in strict code": (
        "(() => { P[7] = 1; return P[7]; })()",
        "throws TypeError: Failed to set an indexed property [7] on 'PluginArray': Indexed property setter is not supported.",
    ),
    "a name set in strict code": (
        "(() => { P['PDF Viewer'] = 1; return P['PDF Viewer']; })()",
        "throws TypeError: Failed to set a named property 'PDF Viewer' on 'PluginArray': Named property setter is not supported.",
    ),
    "a name set in sloppy code": (
        "String(sloppySet(P, 'PDF Viewer', 1))",
        "gives [object Plugin]",
    ),
    "length set in strict code": (
        "(() => { P.length = 9; return P.length; })()",
        "throws TypeError: Cannot set property length of #<PluginArray> which has only a getter",
    ),
    "an index deleted in strict code": (
        "delete P[0]",
        "throws TypeError: Failed to delete an indexed property [0] from 'PluginArray': Index property deleter is not supported.",
    ),
    "an index deleted in sloppy code": (
        "sloppyDelete(P, 0)",
        "gives false",
    ),
    "an index out of range deleted": (
        "delete P[9]",
        "gives true",
    ),
    "a name deleted in strict code": (
        "delete P['PDF Viewer']",
        "throws TypeError: Failed to delete a named property 'PDF Viewer' from 'PluginArray': Named property deleter is not supported.",
    ),
    "a name deleted in sloppy code": (
        "sloppyDelete(P, 'PDF Viewer')",
        "gives false",
    ),
    "an index defined": (
        "Object.defineProperty(P, '0', {value: 1})",
        "throws TypeError: Failed to set an indexed property [0] on 'PluginArray': Index property setter is not supported.",
    ),
    "an index defined as an accessor": (
        "Object.defineProperty(P, '1', {get() { return 1; }})",
        "throws TypeError: Failed to set an indexed property [1] on 'PluginArray': Accessor properties are not allowed.",
    ),
    "an index out of range defined": (
        "Object.defineProperty(P, '8', {value: 1})",
        "throws TypeError: Failed to set an indexed property [8] on 'PluginArray': Index property setter is not supported.",
    ),
    "an index defined through Reflect": (
        "Reflect.defineProperty(P, '0', {value: 1})",
        "gives true",
    ),
    "a name defined": (
        "Object.defineProperty(P, 'PDF Viewer', {value: 1})",
        "throws TypeError: Failed to set a named property 'PDF Viewer' on 'PluginArray': Named property setter is not supported.",
    ),
    "a name defined through Reflect": (
        "Reflect.defineProperty(P, 'PDF Viewer', {value: 1})",
        "gives true",
    ),
    "extensions prevented": (
        "Object.preventExtensions(P[1])",
        "throws TypeError: Cannot prevent extensions",
    ),
    "an object that inherits from navigator.plugins": (
        "(() => { const o = Object.create(P); return [o[0] === P[0], o['PDF Viewer'] === P[0], '0' in o, Object.keys(o).length].join(); })()",
        "gives true,true,true,0",
    ),
    "item without an argument": (
        "P.item()",
        "throws TypeError: Failed to execute 'item' on 'PluginArray': 1 argument required, but only 0 present.",
    ),
    "namedItem without an argument": (
        "P.namedItem()",
        "throws TypeError: Failed to execute 'namedItem' on 'PluginArray': 1 argument required, but only 0 present.",
    ),
    "item of a string": (
        "P.item('1').name",
        "gives Chrome PDF Viewer",
    ),
    "JSON": (
        "JSON.stringify(P) + JSON.stringify(M[0])",
        'gives {"0":{"0":{},"1":{}},"1":{"0":{},"1":{}},"2":{"0":{},"1":{}},"3":{"0":{},"1":{}},"4":{"0":{},"1":{}}}{}',
    ),
    "String": (
        "[String(P), String(P[0]), String(M), String(M[0])].join()",
        "gives [object PluginArray],[object Plugin],[object MimeTypeArray],[object MimeType]",
    ),
    "enabledPlugin of another object": (
        "Object.getOwnPropertyDescriptor(MimeType.prototype, 'enabledPlugin').get.call(P)",
        "throws TypeError: Illegal invocation",
    ),
    "new PluginArray": (
        "new PluginArray()",
        "throws TypeError: Failed to construct 'PluginArray': Illegal constructor",
    ),
    "a new property beside the named ones": (
        "(() => { M.foo = 1; Object.defineProperty(M, 'bar', {value: 2, configurable: true}); return [M.foo, M.bar, Reflect.ownKeys(M).join()].join('|'); })()",
        "gives 1|2|0,1,foo,bar,application/pdf,text/pdf",
    ),
}


@pytest.fixture(scope="module")
def legacy_platform_objects(program: Path) -> dict[str, str]:
    """What each case gives in the Page, from one script that runs them all in order."""
    entries = ",\n".join(
        f"[{json.dumps(name)}, () => {expression}]"
        for name, (expression, _) in _LEGACY_PLATFORM_OBJECTS_AS_CHROME.items()
    )
    script = (
        f"(() => {{ 'use strict'; {_LEGACY_PLATFORM_OBJECTS_PRELUDE} const cases = [{entries}];"
        " const outcome = (run) => { try { return 'gives ' + String(run()); } catch (e) { return 'throws ' + e; } };"
        " return JSON.stringify(cases.map(([name, run]) => [name, outcome(run)])); })()"
    )
    result = _run(program, "-", stdin=script)
    assert (result.returncode, result.stderr) == (0, "")
    return dict(json.loads(result.stdout))


@pytest.mark.parametrize("name", list(_LEGACY_PLATFORM_OBJECTS_AS_CHROME))
def test_a_legacy_platform_object_is_as_in_chrome(legacy_platform_objects: dict[str, str], name: str) -> None:
    assert legacy_platform_objects[name] == _LEGACY_PLATFORM_OBJECTS_AS_CHROME[name][1]
