"""Tests of the web-platform-tests harness: how it compares a Page's results with Chrome's, and the repository's files
of tests, Chrome's results and the list of differences. They need no native library."""

import json
import shutil
from collections.abc import Sequence
from pathlib import Path

import pytest

from tools.wpt.check import SHORT_SECONDS, check, expected_timeouts, list_differences, run_tests
from tools.wpt.copying import copy_tests
from tools.wpt.counts import count_by, table
from tools.wpt.differences import (
    Difference,
    DifferencesError,
    differences_of,
    differing_subtests,
    dumps_differences,
    flaky,
    flaky_tests,
    loads_differences,
    mismatches,
)
from tools.wpt.files import CHROME_FILE, DIFFERENCES_FILE, TESTS_FILE, WPT_ROOT
from tools.wpt.needs import TESTDRIVER, needs_of
from tools.wpt.parts import DIRECTORIES, PARTS, directory_of, part_of
from tools.wpt.reporter.collect import result_key
from tools.wpt.results import (
    ChromeResult,
    ResultsError,
    RunResult,
    Subtest,
    dumps_chrome_results,
    dumps_results,
    loads_chrome_results,
    loads_results,
    result_from_record,
)
from tools.wpt.server import WptServer
from tools.wpt.tests import ListedTest, ListedTestsError, dumps_tests, loads_tests

TEST = "dom/nodes/Node-textContent.html"


def passing(*names: str) -> tuple[Subtest, ...]:
    return tuple(Subtest(name, "PASS") for name in names)


def chrome(*subtests: Subtest, harness: str = "OK", message: str | None = None) -> ChromeResult:
    return ChromeResult.of(RunResult(TEST, harness, message, subtests))


# What a test belongs to


def test_each_test_belongs_to_one_part_at_most() -> None:
    assert part_of("dom/nodes/MutationObserver-attributes.html") == next(p for p in PARTS if p.ticket == "#273")
    assert part_of("dom/nodes/Node-textContent.html") == next(p for p in PARTS if p.ticket == "#274")
    assert part_of("html/semantics/scripting-1/the-script-element/module/a.html") == next(
        p for p in PARTS if p.ticket == "#272"
    )
    assert part_of("encoding/streams/decode-utf8.any.html") is None
    assert part_of("fetch/http-cache/basic-auth-cache-test.html") is None
    for part in PARTS:
        for prefix in part.include:
            assert [p.ticket for p in PARTS if p.holds(prefix + "x.html")] == [part.ticket]


def test_a_test_is_counted_under_the_longest_place_that_names_it() -> None:
    assert directory_of("dom/nodes/MutationObserver-attributes.html") == "dom/nodes/MutationObserver-"
    assert directory_of("dom/nodes/Node-textContent.html") == "dom/nodes/"
    assert directory_of("fetch/http-cache/basic-auth-cache-test.html") == "fetch/"
    with pytest.raises(ValueError, match="none of the directories"):
        directory_of("css/x.html")


def test_the_copy_takes_each_directory_once() -> None:
    assert "dom/" in DIRECTORIES
    assert "dom/nodes/" not in DIRECTORIES
    assert "service-workers/cache-storage/" in DIRECTORIES
    assert all(not a.startswith(b) for a in DIRECTORIES for b in DIRECTORIES if a != b)


def test_needs_come_from_the_test_and_the_scripts_it_includes(tmp_path: Path) -> None:
    (tmp_path / "a").mkdir()
    (tmp_path / "a" / "test.html").write_text('<script src="helper.js"></script><script>x.offsetWidth</script>')
    (tmp_path / "a" / "helper.js").write_text("window.open('x')")
    (tmp_path / "a" / "plain.html").write_text("<script>1</script>")

    assert needs_of(tmp_path, "a/test.html", [], testdriver=True) == [TESTDRIVER, "layout", "second window"]
    assert needs_of(tmp_path, "a/plain.html", ["/a/helper.js"], testdriver=False) == ["second window"]
    assert needs_of(tmp_path, "a/plain.html", ["../../outside.js"], testdriver=False) == []


# A run's results


def test_a_record_reads_as_testharness_js_gives_it() -> None:
    record = {
        "url": "http://web-platform.test:8001/x.html",
        "harness": {"status": 1, "message": "Uncaught Error: no"},
        "tests": [
            {"name": "a", "status": 0, "message": None},
            {"name": "b", "status": 1, "message": "assert_true: expected true got false"},
            {"name": "a", "status": 4, "message": ""},
            {"name": "c", "status": 2, "message": "Test timed out"},
        ],
    }

    assert result_from_record(TEST, record) == RunResult(
        TEST,
        "ERROR",
        "Uncaught Error: no",
        (
            Subtest("a", "PASS"),
            Subtest("b", "FAIL", "assert_true: expected true got false"),
            Subtest("a#2", "PRECONDITION_FAILED"),
            Subtest("c", "TIMEOUT", "Test timed out"),
        ),
    )


@pytest.mark.parametrize(
    "record",
    [{}, {"harness": {"status": 0}}, {"harness": {"status": 9}, "tests": []}, {"harness": {}, "tests": []}],
)
def test_a_record_the_reporter_does_not_write_is_refused(record: dict[str, object]) -> None:
    with pytest.raises(ResultsError):
        result_from_record(TEST, record)


def test_chrome_keeps_what_does_not_pass_and_how_many_ran() -> None:
    result = RunResult(TEST, "OK", None, (Subtest("a", "PASS"), Subtest("b", "FAIL", "no")))

    assert ChromeResult.of(result) == ChromeResult(TEST, "OK", None, 2, [["b", "FAIL", "no"]])
    assert ChromeResult.of(RunResult(TEST, "TIMEOUT")) == ChromeResult(TEST, "TIMEOUT")


def test_chrome_results_read_back_by_test_with_both_recordings_of_a_flaky_one() -> None:
    first = chrome(*passing("a"))
    second = chrome(Subtest("a", "FAIL", "no"))
    other = ChromeResult("a/b.html", "OK", None, 1)

    text = dumps_chrome_results([first, other, second, first])

    assert loads_chrome_results(text) == {"a/b.html": [other], TEST: [second, first]}
    assert len(text.splitlines()) == 5


def test_a_run_reads_back_as_it_was_written() -> None:
    results = [RunResult(TEST, "OK", None, passing("a")), RunResult("a/b.html", "TIMEOUT")]

    assert loads_results(dumps_results(results)) == results


@pytest.mark.parametrize("text", ["", "{}", '[{"test": "x"}]', '[{"test": "x", "harness": "OK", "nope": 1}]'])
def test_bad_results_are_refused(text: str) -> None:
    with pytest.raises(ResultsError):
        loads_results(text)
    with pytest.raises(ResultsError):
        loads_chrome_results(text)


# Blink's rule


def test_the_same_results_have_no_difference() -> None:
    subtests = (*passing("a"), Subtest("b", "FAIL", "no"))

    assert differences_of([chrome(*subtests)], RunResult(TEST, "OK", None, subtests)) == []


def test_a_subtest_that_fails_only_in_the_page_differs() -> None:
    page = RunResult(TEST, "OK", None, (Subtest("a", "FAIL", "assert_equals: expected 1 but got 0"),))

    assert differences_of([chrome(*passing("a"))], page) == [
        Difference(TEST, "a", "PASS", "FAIL: assert_equals: expected 1 but got 0")
    ]


def test_a_subtest_that_passes_only_in_the_page_differs() -> None:
    page = RunResult(TEST, "OK", None, passing("a"))

    assert differences_of([chrome(Subtest("a", "FAIL", "no"))], page) == [Difference(TEST, "a", "FAIL: no", "PASS")]


def test_a_failure_compares_its_message_only_where_chrome_has_one() -> None:
    page = RunResult(TEST, "OK", None, (Subtest("a", "FAIL", "other"),))

    assert differences_of([chrome(Subtest("a", "FAIL", "no"))], page) == [
        Difference(TEST, "a", "FAIL: no", "FAIL: other")
    ]
    assert differences_of([chrome(Subtest("a", "FAIL"))], page) == []


def test_the_message_of_a_timeout_or_notrun_does_not_count() -> None:
    page = RunResult(TEST, "OK", None, (Subtest("a", "TIMEOUT", "late"), Subtest("b", "NOTRUN", "x")))

    assert differences_of([chrome(Subtest("a", "TIMEOUT", "Test timed out"), Subtest("b", "NOTRUN"))], page) == []


def test_a_listed_subtest_that_did_not_run_differs_and_so_does_the_count() -> None:
    page = RunResult(TEST, "OK", None, passing("a"))

    assert differences_of([chrome(*passing("a"), Subtest("b", "FAIL", "no"))], page) == [
        Difference(TEST, None, "2 subtests", "1 subtests"),
        Difference(TEST, "b", "FAIL: no", None),
    ]


def test_a_harness_error_differs_with_its_message() -> None:
    page = RunResult(TEST, "ERROR", "Uncaught TypeError: x is not a function", passing("a"))

    assert differences_of([chrome(*passing("a"))], page) == [
        Difference(TEST, None, "OK", "ERROR: Uncaught TypeError: x is not a function")
    ]


def test_a_timed_out_test_compares_only_that() -> None:
    assert differences_of([chrome(*passing("a", "b"))], RunResult(TEST, "TIMEOUT")) == [
        Difference(TEST, None, "OK", "TIMEOUT")
    ]
    assert differences_of([chrome(harness="TIMEOUT")], RunResult(TEST, "TIMEOUT")) == []
    assert differences_of([chrome(harness="TIMEOUT")], RunResult(TEST, "OK", None, passing("a"))) == [
        Difference(TEST, None, "TIMEOUT", "OK")
    ]


def test_a_flaky_test_matches_either_recording_subtest_by_subtest() -> None:
    first = chrome(*passing("a"), Subtest("b", "FAIL", "no"), *passing("c"))
    second = chrome(Subtest("a", "FAIL", "no"), *passing("b", "c"))
    page = RunResult(TEST, "OK", None, (Subtest("a", "FAIL", "no"), Subtest("b", "FAIL", "no"), Subtest("c", "FAIL")))

    assert differences_of([first, second], page) == [Difference(TEST, "c", "PASS", "FAIL")]
    assert differences_of([first, second], RunResult(TEST, "OK", None, passing("a", "b", "c"))) == []


def test_no_result_of_chrome_cannot_be_compared() -> None:
    with pytest.raises(ValueError, match="no result of Chrome"):
        differences_of([], RunResult(TEST, "TIMEOUT"))


def test_differing_subtests_count_all_of_chromes_for_a_timeout() -> None:
    recorded = [chrome(*passing("a", "b", "c"))]

    assert differing_subtests(recorded, [Difference(TEST, None, "OK", "TIMEOUT")]) == 3
    assert differing_subtests(recorded, [Difference(TEST, None, "OK", "ERROR: x")]) == 0
    assert (
        differing_subtests(
            recorded, [Difference(TEST, None, "3 subtests", "1 subtests"), Difference(TEST, "a", "PASS", "FAIL")]
        )
        == 3
    )


# The list of differences

DIFFERENCE = Difference(TEST, "a", "PASS", "FAIL: no")
OTHER = Difference("a/b.html", None, "OK", "TIMEOUT")


def test_the_list_reads_back_in_the_order_of_the_tests() -> None:
    text = dumps_differences([DIFFERENCE, OTHER])

    assert loads_differences(text) == [OTHER, DIFFERENCE]
    assert json.loads(text.splitlines()[1].rstrip(",")) == {
        "test": "a/b.html",
        "subtest": None,
        "chrome": "OK",
        "mitto": "TIMEOUT",
    }
    assert dumps_differences([]) == "[]\n"


@pytest.mark.parametrize("text", ["", "{}", '["a"]', '[{"test": "a"}]'])
def test_a_bad_list_is_refused(text: str) -> None:
    with pytest.raises(DifferencesError):
        loads_differences(text)


def test_a_run_holds_only_the_tests_it_ran_to_the_list() -> None:
    ran = {TEST: None}

    assert not mismatches([DIFFERENCE], [DIFFERENCE, OTHER], ran)
    changed = Difference(TEST, "a", "PASS", "FAIL: other")
    found = mismatches([changed], [DIFFERENCE, OTHER], ran)
    assert found.unlisted == [changed]
    assert found.gone == [DIFFERENCE]
    report = str(found)
    assert "Not in the list of differences (1):" in report
    assert f"{TEST}, a\n  Chrome: PASS\n  mitto:  FAIL: other" in report
    assert "In the list of differences, but no longer there (1):" in report


def test_counts_go_by_directory_and_by_part() -> None:
    tests = [
        ListedTest(TEST, False, []),
        ListedTest("dom/nodes/Other.html", False, []),
        ListedTest("dom/nodes/Driven.html", False, [TESTDRIVER]),
    ]
    recorded = {TEST: [chrome(*passing("a", "b"))], "dom/nodes/Other.html": [chrome(*passing("a"))]}
    found = [Difference(TEST, None, "OK", "TIMEOUT")]

    by_directory = count_by(tests, recorded, found)
    assert list(by_directory) == ["dom/nodes/"]
    count = by_directory["dom/nodes/"]
    assert (count.tests, count.not_run, count.differing_tests, count.subtests, count.differing_subtests) == (
        2,
        1,
        1,
        3,
        2,
    )
    assert list(count_by(tests, recorded, found, by_part=True)) == ["#274 Element interfaces"]
    assert list(table(by_directory, "Directory"))[-1] == "| All | 2 | 1 | 1 | 3 | 2 |"


# The server


def test_a_test_opens_on_the_scheme_port_and_name_its_flags_ask_for() -> None:
    server = WptServer(Path("results"))

    assert server.url_for(TEST) == f"http://web-platform.test:8001/{TEST}"
    assert server.url_for("a/b.https.any.html") == "https://web-platform.test:8444/a/b.https.any.html"
    assert server.url_for("a/b.h2.html") == "https://web-platform.test:9000/a/b.h2.html"
    assert server.url_for("a/b.www.html?x") == "http://www.web-platform.test:8001/a/b.www.html?x"
    assert server.url_for("a/b.html?wpt_flags=https") == "https://web-platform.test:8444/a/b.html?wpt_flags=https"


def test_a_result_is_found_whether_or_not_its_url_is_percent_encoded() -> None:
    assert result_key("/a/b.html?timeout hit") == result_key("http://web-platform.test:8001/a/b.html?timeout%20hit#x")
    assert result_key("/a/b.html?x") != result_key("/a/b.html?y")


# The repository's files


def test_the_list_of_tests_reads_back() -> None:
    tests = [ListedTest(TEST, True, ["layout"]), ListedTest("a/b.html", False, [TESTDRIVER])]

    assert loads_tests(dumps_tests(tests)) == tests
    assert [test.timeout for test in tests] == [60.0, 10.0]
    assert [test.runs for test in tests] == [True, False]
    with pytest.raises(ListedTestsError):
        loads_tests('[{"test": "x"}]')


def test_the_copy_holds_each_listed_test() -> None:
    tests = loads_tests(TESTS_FILE.read_text(encoding="utf-8"))

    assert len(tests) > 8000
    assert all(test.test.startswith(DIRECTORIES) for test in tests)
    missing = sorted({_source(test.test.split("?")[0]) for test in tests} - _files())
    assert missing == []


_SCRIPT_PAGES = {
    ".any.worker-module.html": ".any.js",
    ".any.worker.html": ".any.js",
    ".any.html": ".any.js",
    ".window.html": ".window.js",
    ".worker.html": ".worker.js",
}
"""The pages that wptserve makes from a test's script, and the script's suffix."""


def _source(test: str) -> str:
    for page, script in _SCRIPT_PAGES.items():
        if test.endswith(page):
            return test.removesuffix(page) + script
    return test


def _files() -> set[str]:
    return {path.relative_to(WPT_ROOT).as_posix() for path in WPT_ROOT.rglob("*") if path.is_file()}


def test_chrome_has_a_result_for_each_test_that_runs() -> None:
    tests = loads_tests(TESTS_FILE.read_text(encoding="utf-8"))
    recorded = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))

    assert sorted(test.test for test in tests if test.runs) == sorted(recorded)


def test_each_listed_difference_is_of_a_test_that_runs() -> None:
    tests = {test.test for test in loads_tests(TESTS_FILE.read_text(encoding="utf-8")) if test.runs}
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8"))

    assert [difference.test for difference in listed if difference.test not in tests] == []
    assert DIFFERENCES_FILE.read_text(encoding="utf-8") == dumps_differences(listed)


# The copy


def test_the_copy_takes_the_directories_what_they_name_and_the_tools(tmp_path: Path) -> None:
    checkout = tmp_path / "src"
    wpt = checkout / "third_party" / "blink" / "web_tests" / "external" / "wpt"
    shutil.copytree(WPT_ROOT / "tools", checkout / "third_party" / "wpt_tools" / "wpt" / "tools")
    harness = '<script src="/resources/testharness.js"></script>'
    files = {
        "LICENSE.md": "licence",
        "resources/testharness.js": "",
        "dom/a.html": f'{harness}<script src="/images/helper.js"></script><script src="../css/b.js"></script>',
        "dom/a-expected.txt": "Chromium's baseline",
        "dom/OWNERS": "someone",
        "dom/b.any.js": "// META: script=/resources/x.js\ntest(() => {});",
        "dom/c.html": f"{harness}<script src='/resources/testdriver.js'></script>",
        "images/helper.js": "",
        "images/helper.js.headers": "Content-Type: text/javascript",
        "images/unused.js": "",
        "css/b.js": "window.open('x')",
        "css/other.html": "",
    }
    for name, text in files.items():
        (wpt / name).parent.mkdir(parents=True, exist_ok=True)
        (wpt / name).write_text(text)
    destination = tmp_path / "copy"
    (destination / "old").mkdir(parents=True)

    tests = copy_tests(checkout, destination)

    copied = {path.relative_to(destination).as_posix() for path in destination.rglob("*") if path.is_file()}
    assert {name for name in copied if not name.startswith("tools/")} == {
        "LICENSE.md",
        "resources/testharness.js",
        "dom/a.html",
        "dom/b.any.js",
        "dom/c.html",
        "images/helper.js",
        "images/helper.js.headers",
        "css/b.js",
    }
    assert "tools/serve/serve.py" in copied
    assert not any("/tests/" in name for name in copied)
    assert tests == [
        ListedTest("dom/a.html", False, ["second window"]),
        ListedTest("dom/b.any.html", False, []),
        ListedTest("dom/b.any.worker.html", False, []),
        ListedTest("dom/c.html", False, [TESTDRIVER]),
    ]


# A run


class FakeRunner:
    """Gives each test the results it is handed, in turn, and notes how many seconds each run gave it."""

    def __init__(self, results: dict[str, list[RunResult]]) -> None:
        self._results = results
        self.seconds: list[tuple[str, float]] = []

    def __call__(self, plan: Sequence[tuple[str, float]], jobs: int) -> list[RunResult]:
        self.seconds += plan
        return [self._results[test].pop(0) for test, _ in plan]


def test_a_test_that_runs_out_of_time_in_a_page_as_the_list_has_it_gets_a_short_deadline() -> None:
    other, both = "a/b.html", "a/c.html"
    tests = [ListedTest(TEST, True, []), ListedTest(other, False, []), ListedTest(both, True, [])]
    recorded = {TEST: [chrome(*passing("a"))], other: [chrome(*passing("a"))], both: [chrome(harness="TIMEOUT")]}
    listed = [Difference(TEST, None, "OK", "TIMEOUT")]
    runner = FakeRunner({test.test: [RunResult(test.test, "TIMEOUT")] for test in tests})

    run_tests(tests, 2, short=expected_timeouts(recorded, listed), runner=runner)

    assert sorted(runner.seconds) == [(other, 10.0), (both, SHORT_SECONDS), (TEST, SHORT_SECONDS)]
    assert expected_timeouts(recorded, [*listed, Difference(both, "a", "PASS", "FAIL")]) == {TEST}


def test_a_run_that_agrees_with_the_list_passes() -> None:
    recorded = {TEST: [chrome(*passing("a"))]}
    runner = FakeRunner({TEST: [RunResult(TEST, "OK", None, (Subtest("a", "FAIL", "no"),))]})

    outcome = check([ListedTest(TEST, False, [])], recorded, [Difference(TEST, "a", "PASS", "FAIL: no")], 1, runner)

    assert outcome
    assert outcome.agreed_again == []


def test_a_test_that_agrees_with_the_list_when_run_again_passes() -> None:
    recorded = {TEST: [chrome(*passing("a"))]}
    runner = FakeRunner({TEST: [RunResult(TEST, "TIMEOUT"), RunResult(TEST, "OK", None, passing("a"))]})

    outcome = check([ListedTest(TEST, False, [])], recorded, [], 1, runner)

    assert outcome
    assert outcome.agreed_again == [TEST]
    assert outcome.results == [RunResult(TEST, "OK", None, passing("a"))]


def test_a_test_that_disagrees_twice_fails_the_run() -> None:
    recorded = {TEST: [chrome(*passing("a"))], "a/b.html": []}
    runner = FakeRunner({TEST: [RunResult(TEST, "TIMEOUT"), RunResult(TEST, "TIMEOUT")]})

    outcome = check([ListedTest(TEST, False, [])], recorded, [], 1, runner)

    assert not outcome
    assert outcome.changes.unlisted == [Difference(TEST, None, "OK", "TIMEOUT")]


def test_a_test_with_no_result_of_chrome_fails_the_run() -> None:
    runner = FakeRunner({TEST: [RunResult(TEST, "OK")]})

    outcome = check([ListedTest(TEST, False, [])], {}, [], 1, runner)

    assert not outcome
    assert outcome.unrecorded == [TEST]


def test_what_differs_from_run_to_run_in_a_message_has_a_placeholder() -> None:
    record = {
        "harness": {"status": 0, "message": None},
        "tests": [
            {
                "name": "a",
                "status": 1,
                "message": 'expected "Token: b9959e6c-a9e7-43fb-8cfa-c9cf96379fde" but got 4.600000023841858',
            },
            {"name": "b", "status": 1, "message": "expected 4.5 but got 0.25"},
            {"name": "c", "status": 1, "message": "expected a number between 1731580000123 and 1731580000456"},
        ],
    }

    subtests = result_from_record(TEST, record).subtests

    assert [subtest.message for subtest in subtests] == [
        'expected "Token: <uuid>" but got <number>',
        "expected 4.5 but got 0.25",
        "expected a number between <number> and <number>",
    ]


# Flaky tests


def test_runs_that_disagree_on_a_test_mark_it_flaky() -> None:
    recorded = {TEST: [chrome(*passing("a"))], "a/b.html": [chrome(*passing("a"))]}
    other = RunResult("a/b.html", "OK", None, (Subtest("a", "FAIL", "no"),))
    first = [RunResult(TEST, "OK", None, passing("a")), other]
    second = [RunResult(TEST, "TIMEOUT"), other]

    written, unrecorded = list_differences([first, second], recorded, [])

    assert written == [flaky(TEST), Difference("a/b.html", "a", "PASS", "FAIL: no")]
    assert unrecorded == []


def test_a_test_marked_flaky_stays_marked_and_is_not_compared() -> None:
    recorded = {TEST: [chrome(*passing("a"))]}
    run = [RunResult(TEST, "TIMEOUT")]

    written, _ = list_differences([run], recorded, [flaky(TEST)])
    outcome = check([ListedTest(TEST, False, [])], recorded, [flaky(TEST)], 1, FakeRunner({TEST: list(run)}))

    assert written == [flaky(TEST)]
    assert outcome
    assert flaky_tests(written) == {TEST}
    assert differing_subtests(recorded[TEST], [flaky(TEST)]) == 1
