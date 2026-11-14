"""web-platform-tests in real Pages, through the harness and its server.

They need the native library with the Page program beside it, as CI's native job installs it on every Host, and are
skipped elsewhere. CI's wpt job runs every test; these check the harness itself.
"""

from pathlib import Path
from typing import Any

import pytest

from mitto import _native
from tools.wpt.differences import differences_of, loads_differences
from tools.wpt.files import CHROME_FILE, DIFFERENCES_FILE
from tools.wpt.pages import run_in_page
from tools.wpt.results import TIMEOUT, loads_chrome_results
from tools.wpt.server import WptServer

_TESTS = ("encoding/textdecoder-labels.any.html", "encoding/textdecoder-labels.any.worker.html")


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip unless the native library has the Page program beside it."""
    library = _native.find_library()
    assert library is not None
    if not library.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the native library")


@pytest.mark.usefixtures("page_program")
def test_pages_give_what_the_list_says_and_run_out_of_time(tmp_path: Path) -> None:
    # One test with one server: the server takes Chromium's fixed ports, so no two of its tests may run at once, as
    # pytest-xdist's workers run separate tests.
    chrome = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8"))

    with WptServer(tmp_path / "results") as server:
        # wptrunner's timeout for these tests: the worker one runs out of time in a Page.
        results = {test: run_in_page(server, test, 10.0) for test in _TESTS}
        # A page with no test in it: it loads, and testharness.js never runs to report.
        blank = run_in_page(server, "common/blank.html", 1.0)

    for test, result in results.items():
        expected = sorted((d for d in listed if d.test == test), key=str)
        assert sorted(differences_of(chrome[test], result), key=str) == expected, test
    assert blank.harness == TIMEOUT
    assert blank.subtests == ()
