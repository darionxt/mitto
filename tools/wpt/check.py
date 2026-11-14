"""A run of web-platform-tests in Pages, held to Chrome's results and the list of differences."""

import time
from collections.abc import Callable, Collection, Iterable, Mapping, Sequence
from dataclasses import dataclass

from tools.wpt.differences import Difference, Mismatches, differences_of, flaky, flaky_tests, mismatches
from tools.wpt.pages import run_in_pages
from tools.wpt.results import TIMEOUT, ChromeResult, RunResult
from tools.wpt.server import WptServer
from tools.wpt.tests import ListedTest

__all__ = ["SHORT_SECONDS", "Check", "check", "compare", "expected_timeouts", "list_differences", "run_tests"]

SHORT_SECONDS = 3.0
"""How long a run gives a test that runs out of time in a Page as the list has it, rather than its full timeout. One that
now finishes within it shows as a difference. One that would finish later stays as it was until the list is written
again, which gives every test its full timeout."""

_PROGRESS_EVERY = 500

type Runner = Callable[[Sequence[tuple[str, float]], int], Iterable[RunResult]]
"""What runs tests, each with how many seconds it has, so many at once, and gives each result: Pages, by default."""


def _in_pages(plan: Sequence[tuple[str, float]], jobs: int) -> Iterable[RunResult]:
    with WptServer() as server:
        yield from run_in_pages(server, plan, jobs)


def expected_timeouts(chrome: Mapping[str, Sequence[ChromeResult]], listed: Iterable[Difference]) -> set[str]:
    """The tests that run out of time in a Page as the list has it: those that the list says run out of time, and those
    that run out of time in Chrome and have no line in the list."""
    listed = list(listed)
    timeouts = {d.test for d in listed if d.subtest is None and d.mitto == TIMEOUT}
    differing = {d.test for d in listed}
    return timeouts | {
        test
        for test, results in chrome.items()
        if test not in differing and results and all(result.harness == TIMEOUT for result in results)
    }


def run_tests(
    tests: Iterable[ListedTest],
    jobs: int,
    *,
    short: Collection[str] = (),
    runner: Runner = _in_pages,
) -> list[RunResult]:
    """Run tests, `jobs` at once, each with its full timeout, or SHORT_SECONDS for one in `short`, and return the
    results in the order of the tests. It prints how far it has got."""
    plan = [(test.test, SHORT_SECONDS if test.test in short else test.timeout) for test in tests]
    # The longest first, so that the run does not end waiting for one.
    plan.sort(key=lambda item: -item[1])
    results: list[RunResult] = []
    started = time.monotonic()
    for result in runner(plan, jobs):
        results.append(result)
        if len(results) % _PROGRESS_EVERY == 0:
            print(f"{len(results)} of {len(plan)} tests, {time.monotonic() - started:.0f} seconds", flush=True)
    print(f"Ran {len(results)} tests in {time.monotonic() - started:.0f} seconds.", flush=True)
    return sorted(results, key=lambda result: result.test)


def compare(
    results: Iterable[RunResult], chrome: Mapping[str, Sequence[ChromeResult]]
) -> tuple[list[Difference], list[str]]:
    """Compare results with Chrome's, and return the differences, and the tests that Chrome has no result for."""
    found: list[Difference] = []
    unrecorded: list[str] = []
    for result in results:
        recorded = chrome.get(result.test)
        if recorded is None:
            unrecorded.append(result.test)
        else:
            found += differences_of(recorded, result)
    return found, unrecorded


@dataclass(frozen=True, slots=True)
class Check:
    """What a run found against the list.

    Attributes:
        results: Each test's result, the second one's for a test that agreed with the list only when run again.
        found: The differences from Chrome.
        changes: How they differ from the list.
        unrecorded: The tests that Chrome has no result for.
        agreed_again: The tests that agreed with the list only when run again.
    """

    results: list[RunResult]
    found: list[Difference]
    changes: Mismatches
    unrecorded: list[str]
    agreed_again: list[str]

    def __bool__(self) -> bool:
        """Whether the run agrees with the list and Chrome has a result for every test."""
        return not self.changes and not self.unrecorded


def check(
    tests: Sequence[ListedTest],
    chrome: Mapping[str, Sequence[ChromeResult]],
    listed: Sequence[Difference],
    jobs: int,
    runner: Runner = _in_pages,
) -> Check:
    """Run tests and hold them to Chrome's results and the list.

    A busy machine can make a test late once, so each test that disagrees with the list runs again, and its second
    result counts if that one agrees.
    """
    short = expected_timeouts(chrome, listed)
    results = {result.test: result for result in run_tests(tests, jobs, short=short, runner=runner)}
    found, _ = compare(results.values(), chrome)
    changes = mismatches(found, listed, results)
    agreed: list[str] = []
    if changes:
        again = {difference.test for difference in (*changes.unlisted, *changes.gone)}
        second = run_tests([test for test in tests if test.test in again], jobs, short=short, runner=runner)
        for result in second:
            if not mismatches(compare([result], chrome)[0], listed, {result.test: result}):
                results[result.test] = result
                agreed.append(result.test)
    ordered = sorted(results.values(), key=lambda result: result.test)
    found, unrecorded = compare(ordered, chrome)
    return Check(ordered, found, mismatches(found, listed, results), unrecorded, agreed)


def list_differences(
    runs: Iterable[Iterable[RunResult]], chrome: Mapping[str, Sequence[ChromeResult]], listed: Iterable[Difference]
) -> tuple[list[Difference], list[str]]:
    """Write the list from one or more runs of the same tests, such as one on each Host, and return it with the tests
    that Chrome has no result for.

    A test whose runs disagree is flaky: the list marks it, and holds none of its differences. A test that the list
    marks already stays marked, since agreeing runs do not show that it no longer varies.
    """
    by_test: dict[str, set[frozenset[Difference]]] = {}
    unrecorded: set[str] = set()
    for run in runs:
        results = list(run)
        found, missing = compare(results, chrome)
        unrecorded.update(missing)
        ran: dict[str, set[Difference]] = {result.test: set() for result in results}
        for difference in found:
            ran[difference.test].add(difference)
        for test, differences in ran.items():
            by_test.setdefault(test, set()).add(frozenset(differences))
    marked = flaky_tests(listed) | {test for test, seen in by_test.items() if len(seen) > 1}
    written = [flaky(test) for test in sorted(marked)]
    for test, seen in by_test.items():
        if test not in marked and test not in unrecorded:
            written += next(iter(seen))
    return written, sorted(unrecorded)
