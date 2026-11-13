"""Tests of querying and walking a Page's tree against Chrome: selectors, collections, ranges and traversal.

tests/query_cases.js runs in the Page program on the markup of _PAGE, which is the page that Chrome 157.0.8103.51 ran it
on from a new profile, and each part of its record is compared with Chrome's in tests/data/query_cases.json: which
selectors parse and the error of each that does not, what each matches in a fixture of forms, links, directions,
languages and foreign content, the collections of getElementsByTagName() and the like and document.all's names, and what
Range, StaticRange, TreeWalker and NodeIterator do and throw. The selector parser's own fuzz target is
native/test/css_selector_fuzzer.cc.

The tests run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_document.py does.
"""

import json
import os
import subprocess
from pathlib import Path
from typing import Any, cast

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_CASES = Path(__file__).parent / "query_cases.js"
_AS_CHROME = Path(__file__).parent / "data" / "query_cases.json"

# The page that Chrome ran the cases on, whose script fetched them and sent back their record. The Page program builds
# the same tree, script element included, and runs the cases itself.
_PAGE = (
    "<!doctype html><html><head><title>q</title></head><body><script>"
    "fetch('/cases.js').then(r => r.text()).then(t => fetch('/result', {method: 'POST', body: (0, eval)(t)}));"
    "</script></body></html>"
)


@pytest.fixture(scope="module")
def record() -> dict[str, Any]:
    """The record of the cases in the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    result = subprocess.run(
        [path, f"--html={_PAGE}", _CASES.read_text(encoding="utf-8")],
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    parsed: dict[str, Any] = json.loads(result.stdout)
    return parsed


def _as_chrome() -> dict[str, Any]:
    cases: dict[str, Any] = json.loads(_AS_CHROME.read_text(encoding="utf-8"))["cases"]
    return cases


@pytest.mark.parametrize("part", list(_as_chrome()))
def test_queries_and_traversal_give_what_chrome_gives(record: dict[str, Any], part: str) -> None:
    expected: Any = _as_chrome()[part]
    actual: Any = record[part]
    if isinstance(expected, list) and isinstance(actual, list):
        # Item by item, so that a failure names the first case that differs.
        pairs = zip(cast(list[Any], actual), cast(list[Any], expected), strict=False)
        for index, (got, want) in enumerate(pairs):
            assert (index, got) == (index, want)
    assert actual == expected
