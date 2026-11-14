"""How many tests and subtests differ from Chrome's, in each directory of web-platform-tests and each part of M8."""

from collections import defaultdict
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass

from tools.wpt.differences import Difference, differing_subtests
from tools.wpt.parts import PARTS, directory_of, part_of
from tools.wpt.results import ChromeResult
from tools.wpt.tests import ListedTest

__all__ = ["Count", "count_by", "table"]


@dataclass(slots=True)
class Count:
    """What one directory or part holds.

    Attributes:
        tests: The tests that run.
        not_run: The tests that do not, since they need testdriver.js.
        differing_tests: The tests with a difference.
        subtests: The subtests that Chrome ran, of the first recording.
        differing_subtests: The subtests that differ, as differing_subtests() counts them.
    """

    tests: int = 0
    not_run: int = 0
    differing_tests: int = 0
    subtests: int = 0
    differing_subtests: int = 0


def count_by(
    tests: Iterable[ListedTest],
    chrome: Mapping[str, Sequence[ChromeResult]],
    differences: Iterable[Difference],
    *,
    by_part: bool = False,
) -> dict[str, Count]:
    """Count the tests and their differences by directory, as #260's table names them, or by part of M8."""
    found: defaultdict[str, list[Difference]] = defaultdict(list)
    for difference in differences:
        found[difference.test].append(difference)
    counts: defaultdict[str, Count] = defaultdict(Count)
    for test in tests:
        key = _part_key(test.test) if by_part else directory_of(test.test)
        count = counts[key]
        if not test.runs:
            count.not_run += 1
            continue
        count.tests += 1
        recorded = chrome.get(test.test, [])
        if recorded:
            count.subtests += recorded[0].subtests
        if found[test.test]:
            count.differing_tests += 1
            count.differing_subtests += differing_subtests(recorded, found[test.test])
    order = [f"{part.ticket} {part.title}" for part in PARTS] if by_part else sorted(counts)
    return {key: counts[key] for key in [*order, *sorted(set(counts) - set(order))] if key in counts}


def _part_key(test: str) -> str:
    part = part_of(test)
    return "none" if part is None else f"{part.ticket} {part.title}"


def table(counts: Mapping[str, Count], heading: str) -> Iterator[str]:
    """Yield a Markdown table of counts, with a total."""
    yield f"| {heading} | Tests | Not run | Tests that differ | Chrome's subtests | Subtests that differ |"
    yield "| --- | ---: | ---: | ---: | ---: | ---: |"
    total = Count()
    for key, count in counts.items():
        yield _row(f"`{key}`" if "/" in key else key, count)
        total.tests += count.tests
        total.not_run += count.not_run
        total.differing_tests += count.differing_tests
        total.subtests += count.subtests
        total.differing_subtests += count.differing_subtests
    yield _row("All", total)


def _row(name: str, count: Count) -> str:
    return (
        f"| {name} | {count.tests:,} | {count.not_run:,} | {count.differing_tests:,} | {count.subtests:,} | "
        f"{count.differing_subtests:,} |"
    )
