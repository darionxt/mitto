"""Tests that the probe, tools/capture/probe.js, runs to its end in a Page and gives the same Normal Form twice in a row.

A Parity Check gives the probe to a Page, so a Page that cannot finish it, or that it changes, fails here first. What the
record says is the Parity Checks' to compare with Chrome's (docs/runtime.md, "The probe").

The tests through a Session need the native library with the Page program beside it, as CI's native job installs it on
every Host. The test of the Page program alone runs where MITTO_PAGE_PROGRAM names it, as in the Runtime's builds with
AddressSanitizer and UndefinedBehaviorSanitizer.
"""

import os
import subprocess
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tools.capture.probe import RecordValue, dumps_record, probe_script, read_record
from tools.capture.script_normal_form import script_normal_form

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
# A Page runs the probe in well under a second today. Chrome takes about a tenth of one for all it exposes, which a Page
# with bindings for every interface does too, and a sanitizer's build several times as long.
_TIMEOUT_SECONDS = 120


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session whose library has the Page program beside it, or skip."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _run_in_a_page(session: mitto.Session) -> dict[str, RecordValue]:
    page: Page
    with session._open_page("about:blank") as page:  # pyright: ignore[reportPrivateUsage]
        record = read_record(page.evaluate(probe_script(), timeout=_TIMEOUT_SECONDS))
        # It leaves nothing that a page or the Session could see: no console message, and no promise rejected with no
        # handler, such as a check's that a Known Gap rejects.
        assert (page.console_messages, page.uncaught_errors) == ([], [])
    return record


def _run_in_the_page_program(program: Path) -> dict[str, RecordValue]:
    # From standard input: the probe is longer than a command line holds on Windows.
    result = subprocess.run(
        [program, "-"],
        input=probe_script(),
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    return read_record(result.stdout)


def _assert_whole(record: dict[str, RecordValue]) -> None:
    objects = record["objects"]
    assert isinstance(objects, list)
    assert isinstance(objects[0], dict)
    assert objects[0]["path"] == "globalThis"
    assert "capped" not in record
    assert list(record) == ["format", "objects", "values", "reports"]
    # The DOM's interface prototypes have Node.prototype on their chain, as nodes do, and are walked all the same.
    paths = {entry["path"] for entry in objects if isinstance(entry, dict) and isinstance(entry["path"], str)}
    assert {"Node.prototype", "Element.prototype", "HTMLElement.prototype", "Text.prototype"} <= paths


def test_the_probe_runs_to_its_end_in_a_page_and_gives_the_same_record_twice(session: mitto.Session) -> None:
    first = _run_in_a_page(session)
    second = _run_in_a_page(session)

    _assert_whole(first)
    # about:blank has no origin of its own to put behind placeholders.
    assert script_normal_form(second, None) == script_normal_form(first, None)


def test_the_probe_runs_to_its_end_in_the_page_program() -> None:
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")

    first = _run_in_the_page_program(Path(path))
    second = _run_in_the_page_program(Path(path))

    _assert_whole(first)
    assert script_normal_form(second, None) == script_normal_form(first, None)


def _record(*objects: dict[str, RecordValue]) -> dict[str, RecordValue]:
    return {"format": 1, "objects": list(objects), "values": {}, "reports": []}


def test_a_record_is_written_with_a_line_for_each_property_getter_check_and_value() -> None:
    record = _record(
        {
            "path": "globalThis",
            "kind": "instance",
            "own": [["a", "w-c", 1], ["b", "-c", ["function", "get b", 0], None]],
            "get": [["b", "x"]],
        },
        {"path": "f", "kind": "function", "new": ["throws", "TypeError: no"]},
    )
    record["values"] = {"one": {"value": [1, 2]}, "two": {"throws": "TypeError: no"}}

    text = dumps_record(record)

    assert read_record(text) == record
    assert text.splitlines() == [
        "{",
        '"format": 1,',
        '"objects": [',
        '{"path": "globalThis", "kind": "instance",',
        ' "own": [',
        '  ["a", "w-c", 1],',
        '  ["b", "-c", ["function", "get b", 0], null]',
        " ],",
        ' "get": [',
        '  ["b", "x"]',
        " ]",
        "},",
        '{"path": "f", "kind": "function", "new": ["throws", "TypeError: no"]}',
        "],",
        '"values": {',
        '"one": {"value": [1, 2]},',
        '"two": {"throws": "TypeError: no"}',
        "},",
        '"reports": []',
        "}",
    ]


@pytest.mark.parametrize("text", ["[]", '{"format": 2, "objects": []}', '{"format": 1}'])
def test_what_is_no_record_is_refused(text: str) -> None:
    with pytest.raises(ValueError, match="not a record of the probe"):
        read_record(text)
