"""A test's results, as a browser's run gives them, and Chrome's results as the repository keeps them."""

import json
import re
from collections import defaultdict
from collections.abc import Iterable, Mapping
from dataclasses import asdict, dataclass, field
from typing import Any, cast

__all__ = [
    "CRASH",
    "OK",
    "PASS",
    "TIMEOUT",
    "ChromeResult",
    "ResultsError",
    "RunResult",
    "Subtest",
    "dumps_chrome_results",
    "dumps_results",
    "loads_chrome_results",
    "loads_results",
    "result_from_record",
]

PASS = "PASS"
OK = "OK"
TIMEOUT = "TIMEOUT"
CRASH = "CRASH"
"""The harness status of a test whose browser ended before it reported: a Page's process, or Chrome's renderer."""

_SUBTEST_STATUSES = ("PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED")
"""testharness.js's statuses of a subtest, by their number."""

_HARNESS_STATUSES = ("OK", "ERROR", "TIMEOUT", "PRECONDITION_FAILED")
"""testharness.js's statuses of the harness, by their number."""

_WITH_MESSAGE = frozenset({"FAIL", "PRECONDITION_FAILED", "ERROR"})
"""The statuses whose message Blink compares."""


class ResultsError(Exception):
    """Results cannot be read."""


@dataclass(frozen=True, slots=True)
class Subtest:
    """One subtest's result.

    Attributes:
        name: Its name. A name that a test gives twice is numbered from the second on, as "name#2".
        status: "PASS", "FAIL", "TIMEOUT", "NOTRUN" or "PRECONDITION_FAILED".
        message: What testharness.js says of a status other than "PASS", if anything.
    """

    name: str
    status: str
    message: str | None = None

    @property
    def outcome(self) -> str:
        """The status, with the message of a status whose message counts, as "FAIL: assert_true: expected true"."""
        return _outcome(self.status, self.message)


@dataclass(frozen=True, slots=True)
class RunResult:
    """What one run of a test gave.

    Attributes:
        test: The test's address from the root of web-platform-tests.
        harness: testharness.js's status of the harness, "TIMEOUT" when the test reported nothing in time, or "CRASH".
        message: What testharness.js says of an error of the harness, or what the runner says of its own.
        subtests: Each subtest's result, in the order that testharness.js gives.
    """

    test: str
    harness: str
    message: str | None = None
    subtests: tuple[Subtest, ...] = ()

    @property
    def outcome(self) -> str:
        """The harness status, with its message where the message counts."""
        return _outcome(self.harness, self.message)

    @property
    def reported(self) -> bool:
        """Whether the test reported its subtests, rather than running out of time or ending."""
        return self.harness not in (TIMEOUT, CRASH)


@dataclass(frozen=True, slots=True)
class ChromeResult:
    """Chrome's result for a test, as Blink keeps a baseline: what does not pass.

    Attributes:
        test: The test's address from the root of web-platform-tests.
        harness: The harness status, as RunResult has it.
        message: What testharness.js says of an error of the harness.
        subtests: How many subtests ran.
        not_passing: Each subtest whose status is not "PASS", as [name, status, message].
    """

    test: str
    harness: str
    message: str | None = None
    subtests: int = 0
    not_passing: list[list[str | None]] = field(default_factory=list[list[str | None]])

    @classmethod
    def of(cls, result: RunResult) -> "ChromeResult":
        """Keep what a run of Chrome gave for a test."""
        if not result.reported:
            return cls(result.test, result.harness, result.message)
        failing = [
            [subtest.name, subtest.status, subtest.message] for subtest in result.subtests if subtest.status != PASS
        ]
        return cls(result.test, result.harness, result.message, len(result.subtests), failing)

    def not_passing_by_name(self) -> dict[str, Subtest]:
        """The subtests that do not pass, by name."""
        return {str(name): Subtest(str(name), str(status), message) for name, status, message in self.not_passing}

    @property
    def outcome(self) -> str:
        """The harness status, with its message where the message counts."""
        return _outcome(self.harness, self.message)

    @property
    def reported(self) -> bool:
        """Whether the test reported its subtests."""
        return self.harness not in (TIMEOUT, CRASH)


def _outcome(status: str, message: str | None) -> str:
    return f"{status}: {message}" if message and status in _WITH_MESSAGE else status


def result_from_record(test: str, record: Mapping[str, Any]) -> RunResult:
    """Read what mitto's testharnessreport.js posted for a test.

    Raises:
        ResultsError: The record is not one that the reporter writes.
    """
    try:
        harness = cast(Mapping[str, Any], record["harness"])
        subtests: list[Subtest] = []
        seen: defaultdict[str, int] = defaultdict(int)
        for entry in cast(list[Mapping[str, Any]], record["tests"]):
            name = str(entry["name"])
            seen[name] += 1
            numbered = name if seen[name] == 1 else f"{name}#{seen[name]}"
            status = _SUBTEST_STATUSES[int(entry["status"])]
            subtests.append(Subtest(numbered, status, None if status == PASS else _message(entry.get("message"))))
        status = _HARNESS_STATUSES[int(harness["status"])]
        return RunResult(test, status, None if status == OK else _message(harness.get("message")), tuple(subtests))
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise ResultsError(f"{test}: not a record of the reporter: {error!r}") from error


_RANDOM_PARTS = (
    (re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"), "<uuid>"),
    (re.compile(r"\d+\.\d{8,}"), "<number>"),
    (re.compile(r"\b\d{13,}\b"), "<number>"),
)
"""What a test puts in a message that differs from run to run, and its placeholder: a token, such as one that a handler
keys a stash with, a number with eight decimals or more, such as Math.random()'s or a time in milliseconds, and a whole
number of thirteen digits or more, such as Date.now()'s."""


def _message(value: object) -> str | None:
    if value is None or value == "":
        return None
    message = str(value)
    for pattern, placeholder in _RANDOM_PARTS:
        message = pattern.sub(placeholder, message)
    return message


def dumps_chrome_results(results: Iterable[ChromeResult]) -> str:
    """Write Chrome's results, one per line in the order of the tests. A test that two recordings disagree on, which is
    flaky, has a line for each."""
    unique = {json.dumps(asdict(result)): result for result in results}
    ordered = sorted(unique.items(), key=lambda item: (item[1].test, item[0]))
    lines = [text for text, _ in ordered]
    return "[\n" + ",\n".join(lines) + "\n]\n" if lines else "[]\n"


def loads_chrome_results(text: str) -> dict[str, list[ChromeResult]]:
    """Read Chrome's results, by test.

    Raises:
        ResultsError: The text is not Chrome's results.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        by_test: dict[str, list[ChromeResult]] = {}
        for entry in cast(list[dict[str, Any]], entries):
            result = ChromeResult(**entry)
            by_test.setdefault(result.test, []).append(result)
        return by_test
    except (ValueError, TypeError) as error:
        raise ResultsError(f"not Chrome's results: {error}") from error


def dumps_results(results: Iterable[RunResult]) -> str:
    """Write the results of a run, one test per line, as a run in CI keeps them for the list to be written from."""
    lines = [json.dumps(asdict(result)) for result in results]
    return "[\n" + ",\n".join(lines) + "\n]\n" if lines else "[]\n"


def loads_results(text: str) -> list[RunResult]:
    """Read the results of a run.

    Raises:
        ResultsError: The text is not the results of a run.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        results: list[RunResult] = []
        for entry in cast(list[dict[str, Any]], entries):
            subtests = tuple(Subtest(**subtest) for subtest in cast(list[dict[str, Any]], entry.pop("subtests", [])))
            results.append(RunResult(**entry, subtests=subtests))
        return results
    except (ValueError, TypeError) as error:
        raise ResultsError(f"not the results of a run: {error}") from error
