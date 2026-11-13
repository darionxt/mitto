"""Tests of the DOM's elements in the Page program: the interface each name gets, namespaces and document fragments.

tests/element_cases.js makes elements of every namespace, sets and reads their attributes, looks namespaces up and moves
document fragments and template contents around, and returns what each case gave or threw. The record of what Chrome
157 gave for the same script is tests/data/element_cases.json, taken from headless Chrome over HTTP, as the cases of
tests/test_page_common_members.py were. These run where MITTO_PAGE_PROGRAM names the Page program, as
tests/test_page_document.py does.
"""

import json
import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_TESTS = Path(__file__).resolve().parent
_CASES = _TESTS / "element_cases.js"
_CHROME = _TESTS / "data" / "element_cases.json"
# The document the cases run in, as Chrome's case page has it, so that lookups on the document and its body agree.
_PAGE = "<!DOCTYPE html><html><head><title>t</title></head><body></body></html>"


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


@pytest.fixture(scope="module")
def page_cases(program: Path) -> dict[str, str]:
    """Return what each case gave in the Page program."""
    result = subprocess.run(
        [program, f"--html={_PAGE}", _CASES.read_text(encoding="utf-8")],
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    cases: dict[str, str] = json.loads(result.stdout)
    return cases


def _chrome_cases() -> dict[str, str]:
    record = json.loads(_CHROME.read_text(encoding="utf-8"))
    cases: dict[str, str] = record["cases"]
    return cases


_CHROME_CASES = _chrome_cases()


@pytest.mark.parametrize("name", list(_CHROME_CASES))
def test_a_case_gives_what_chrome_gave(page_cases: dict[str, str], name: str) -> None:
    assert page_cases.get(name) == _CHROME_CASES[name]


def test_the_page_runs_every_case_that_chrome_ran(page_cases: dict[str, str]) -> None:
    assert sorted(page_cases) == sorted(_CHROME_CASES)
