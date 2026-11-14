"""Where a Page's results differ from Chrome's, by Blink's rule, and the list of those differences that the repository
keeps, as the Parity Checks keep their Known Gaps."""

import json
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from typing import Any, cast

from tools.wpt.results import CRASH, PASS, TIMEOUT, ChromeResult, RunResult, Subtest

__all__ = [
    "FLAKY",
    "TICKET",
    "Difference",
    "DifferencesError",
    "Mismatches",
    "differences_of",
    "dumps_differences",
    "flaky",
    "flaky_tests",
    "loads_differences",
    "mismatches",
]

_NOTHING = "(nothing)"

FLAKY = "FLAKY"
"""What the list says a Page gives for a test whose result varies between runs or Hosts."""

TICKET = re.compile(r"#[1-9][0-9]*")
"""What a ticket that owns a difference looks like, such as "#283"."""


class DifferencesError(Exception):
    """A list of differences cannot be read."""


@dataclass(frozen=True, slots=True)
class Difference:
    """One place where a Page's result for a test differs from Chrome's.

    Attributes:
        test: The test's address from the root of web-platform-tests.
        subtest: The subtest's name, or None for the test as a whole: its harness status, or how many subtests ran.
        chrome: Chrome's outcome, such as "PASS", "FAIL: assert_true: expected true", "TIMEOUT" or "222 subtests", or
            None where Chrome had no such subtest.
        mitto: The Page's outcome, in the same form, or None where the Page did not run such a subtest.
        owner: The ticket whose missing behaviour causes it, such as "#283", where that is not the ticket of the
            test's part of M8 (tools.wpt.owners). It takes no part in comparing differences.
    """

    test: str
    subtest: str | None
    chrome: str | None
    mitto: str | None
    owner: str | None = field(default=None, compare=False)

    def __str__(self) -> str:
        where = self.test if self.subtest is None else f"{self.test}, {self.subtest}"
        text = f"{where}\n  Chrome: {self.chrome or _NOTHING}\n  mitto:  {self.mitto or _NOTHING}"
        return text if self.owner is None else f"{text}\n  owner:  {self.owner}"


def differences_of(chrome: Iterable[ChromeResult], result: RunResult) -> list[Difference]:
    """Compare a Page's result for a test with Chrome's, by Blink's rule, and return each difference.

    Blink's rule: the harness status must be Chrome's, with its message for an error; each subtest's status must be
    Chrome's, PASS where Chrome's baseline does not list it; a FAIL or PRECONDITION_FAILED must have Chrome's message where
    Chrome's has one; and every subtest that Chrome listed must have run. As Chrome's baselines do not name the subtests
    that pass, a different number of subtests is a difference too. A test that ran out of time or ended compares only
    whether the other did the same, since how far it got depends on when it stopped.

    Chrome's result has two entries for a test whose two recordings differed, which is flaky, and a Page's result then
    matches either: a subtest differs only where it differs from both.
    """
    each = [_differences_from(recorded, result) for recorded in chrome]
    if not each:
        raise ValueError(f"{result.test}: no result of Chrome to compare with")
    if any(not found for found in each):
        return []
    places = [{_place(difference) for difference in found} for found in each]
    in_every = places[0].intersection(*places[1:])
    return [difference for difference in each[0] if _place(difference) in in_every]


def _place(difference: Difference) -> tuple[str | None, bool]:
    """Where a difference is: its subtest, or for the test as a whole, whether it is the number of subtests."""
    return difference.subtest, difference.subtest is None and str(difference.chrome).endswith(" subtests")


def _differences_from(chrome: ChromeResult, result: RunResult) -> list[Difference]:
    test = result.test
    if not chrome.reported or not result.reported:
        same = chrome.harness == result.harness
        return [] if same else [Difference(test, None, chrome.outcome, result.outcome)]
    found: list[Difference] = []
    if chrome.outcome != result.outcome:
        found.append(Difference(test, None, chrome.outcome, result.outcome))
    if chrome.subtests != len(result.subtests):
        found.append(Difference(test, None, f"{chrome.subtests} subtests", f"{len(result.subtests)} subtests"))
    ran = {subtest.name: subtest for subtest in result.subtests}
    expected = chrome.not_passing_by_name()
    for name, subtest in expected.items():
        actual = ran.get(name)
        if actual is None:
            found.append(Difference(test, name, subtest.outcome, None))
        elif not _matches(subtest, actual):
            found.append(Difference(test, name, subtest.outcome, actual.outcome))
    for subtest in result.subtests:
        if subtest.status != PASS and subtest.name not in expected:
            found.append(Difference(test, subtest.name, PASS, subtest.outcome))
    return found


def _matches(chrome: Subtest, page: Subtest) -> bool:
    """Whether a Page's subtest matches Chrome's listed one: the same status, and the same message where Chrome's status
    has one whose message counts."""
    if chrome.status != page.status:
        return False
    return chrome.outcome == chrome.status or chrome.outcome == page.outcome


def differing_subtests(chrome: Iterable[ChromeResult], found: Iterable[Difference]) -> int:
    """How many of a test's subtests differ, for the counts by directory: every one of Chrome's when a test ran out of
    time, ended or is flaky on one side only, else each subtest that differs and each that one side ran and the other
    did not."""
    whole = max((result.subtests for result in chrome if result.reported), default=0)
    all_reported = all(result.reported for result in chrome)
    count = 0
    for difference in found:
        if difference.subtest is not None:
            count += 1
        elif _place(difference)[1]:
            count += abs(int(str(difference.chrome).split()[0]) - int(str(difference.mitto).split()[0]))
        elif not all_reported or difference.mitto in _WHOLE_TEST:
            count += max(1, whole)
    return count


_WHOLE_TEST = frozenset({TIMEOUT, CRASH, FLAKY})
"""What a Page gives for a test as a whole, with no subtest to compare."""


def dumps_differences(differences: Iterable[Difference]) -> str:
    """Write a list of differences as text, one per line in the order of the tests, so that one that closes is one line
    that goes."""
    lines = [json.dumps(_entry(difference)) for difference in sorted(differences, key=_order)]
    return "[\n" + ",\n".join(lines) + "\n]\n" if lines else "[]\n"


def _entry(difference: Difference) -> dict[str, str | None]:
    entry = {
        "test": difference.test,
        "subtest": difference.subtest,
        "chrome": difference.chrome,
        "mitto": difference.mitto,
    }
    # Only a line that another ticket owns says so.
    if difference.owner is not None:
        entry["owner"] = difference.owner
    return entry


def loads_differences(text: str) -> list[Difference]:
    """Read a list of differences.

    Raises:
        DifferencesError: The text is not a list of differences.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        differences = [Difference(**entry) for entry in cast(list[dict[str, Any]], entries)]
    except (ValueError, TypeError) as error:
        raise DifferencesError(f"not a list of differences: {error}") from error
    for difference in differences:
        if difference.owner is not None and not TICKET.fullmatch(str(difference.owner)):
            raise DifferencesError(f'{difference.test}: owner {difference.owner!r} is not a ticket such as "#283"')
    return differences


@dataclass(frozen=True, slots=True)
class Mismatches:
    """How the differences a run found differ from the list. It is false when they agree.

    Attributes:
        unlisted: The differences that were found but are not in the list.
        gone: The differences that are in the list but were not found.
    """

    unlisted: list[Difference]
    gone: list[Difference]

    def __bool__(self) -> bool:
        return bool(self.unlisted or self.gone)

    def __str__(self) -> str:
        sections: list[str] = []
        if self.unlisted:
            sections.append(f"Not in the list of differences ({len(self.unlisted)}):")
            sections += [str(difference) for difference in self.unlisted]
        if self.gone:
            sections.append(f"In the list of differences, but no longer there ({len(self.gone)}):")
            sections += [str(difference) for difference in self.gone]
        return "\n".join(sections)


def flaky(test: str) -> Difference:
    """The line of the list that marks a test whose result in a Page varies between runs or Hosts, which a run then
    does not compare."""
    return Difference(test, None, None, FLAKY)


def flaky_tests(listed: Iterable[Difference]) -> set[str]:
    """The tests that the list marks as flaky."""
    return {difference.test for difference in listed if difference == flaky(difference.test)}


def mismatches(found: Iterable[Difference], listed: Iterable[Difference], tests: Mapping[str, object]) -> Mismatches:
    """Compare the differences a run found with the list, for the tests that the run ran, which `tests` holds, but those
    that the list marks as flaky. A difference whose outcome has changed counts as both."""
    listed = list(listed)
    marked = flaky_tests(listed)
    found_set = {difference for difference in found if difference.test not in marked}
    listed_set = {difference for difference in listed if difference.test in tests and difference.test not in marked}
    return Mismatches(
        unlisted=sorted(found_set - listed_set, key=_order),
        gone=sorted(listed_set - found_set, key=_order),
    )


def _order(difference: Difference) -> tuple[str, bool, str, str]:
    return (difference.test, difference.subtest is not None, difference.subtest or "", difference.chrome or "")
