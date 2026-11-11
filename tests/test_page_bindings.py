"""Tests of the bindings in the Page program, which the generator in native/bindings writes from Blink's Web IDL files.

These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_program.py does. What a page sees of the
interfaces the Runtime implements is compared with what real Chrome of the Engine Release showed: tests/runtime/chrome
holds a record for each Chrome build, which probe.js made there through record.html (docs/runtime.md says how), and the
same probe runs here.
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
_REPOSITORY = Path(__file__).resolve().parents[1]
_RECORDINGS = _REPOSITORY / "tests" / "runtime" / "chrome"
_ENGINE_BUILD = (_REPOSITORY / "tools" / "workspace" / "chromium_tag").read_text(encoding="utf-8").strip()

# The interfaces that the probe records, those that the Runtime implements. A member whose behaviour the Runtime does
# not have yet, a Known Gap that native/bindings/interfaces.gni lists, has Chrome's shape all the same.
_INTERFACES = [
    "CharacterData",
    "Comment",
    "DOMException",
    "Document",
    "DocumentType",
    "Element",
    "Event",
    "EventTarget",
    "HTMLBRElement",
    "HTMLBodyElement",
    "HTMLDivElement",
    "HTMLDocument",
    "HTMLElement",
    "HTMLHeadElement",
    "HTMLHeadingElement",
    "HTMLHtmlElement",
    "HTMLLIElement",
    "HTMLLinkElement",
    "HTMLMetaElement",
    "HTMLOListElement",
    "HTMLParagraphElement",
    "HTMLPreElement",
    "HTMLScriptElement",
    "HTMLSpanElement",
    "HTMLStyleElement",
    "HTMLTitleElement",
    "HTMLUListElement",
    "HTMLUnknownElement",
    "Node",
    "PopStateEvent",
    "Text",
    "Window",
]


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


@pytest.fixture(scope="module")
def chrome() -> dict[str, Any]:
    """What Chrome of the Engine Release recorded."""
    recording: dict[str, Any] = json.loads((_RECORDINGS / f"{_ENGINE_BUILD}.json").read_text(encoding="utf-8"))
    assert recording["chrome"] == _ENGINE_BUILD
    return recording["probe"]


@pytest.fixture(scope="module")
def page(program: Path) -> dict[str, Any]:
    """What the Page program records with the same probe, which it reads from standard input: the probe is longer than
    a command line holds on Windows."""
    result = _run(program, "-", stdin=(_RECORDINGS / "probe.js").read_text(encoding="utf-8"))
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def test_there_is_a_recording_of_the_engine_release() -> None:
    assert (_RECORDINGS / f"{_ENGINE_BUILD}.json").is_file()


@pytest.mark.parametrize("name", _INTERFACES)
@pytest.mark.parametrize(
    "part",
    [
        "global",
        "interfaceObject",
        "prototype",
        "prototypeParent",
        "interfaceObjectParent",
        "toStringTag",
        "callWithoutNew",
        "constructWithoutArguments",
        "wrongReceiver",
    ],
)
def test_an_interface_is_as_chrome_has_it(chrome: dict[str, Any], page: dict[str, Any], name: str, part: str) -> None:
    # Own properties in order, with their attributes, names and lengths, and what calls with the wrong receiver throw.
    assert page["interfaces"][name][part] == chrome["interfaces"][name][part]


@pytest.mark.parametrize("name", sorted(set(_INTERFACES) - {"CharacterData", "DocumentType", "Node", "Window"}))
def test_an_instance_has_chromes_own_properties(chrome: dict[str, Any], page: dict[str, Any], name: str) -> None:
    # CharacterData and Node have no instances of their own, a page makes no doctype without a parser of documents,
    # and Window's instance holds every interface object, which the Page lacks until M7.
    assert page["instances"][name] == chrome["instances"][name]


def test_the_cases_go_as_in_chrome(chrome: dict[str, Any], page: dict[str, Any]) -> None:
    assert [name for name, _ in page["cases"]] == [name for name, _ in chrome["cases"]]
    for (name, outcome), (_, expected) in zip(page["cases"], chrome["cases"], strict=True):
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
