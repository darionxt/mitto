"""The list of tests in the copy of web-platform-tests, which the copy tool writes."""

import json
from collections.abc import Iterable
from dataclasses import asdict, dataclass
from typing import Any, cast

from tools.wpt.needs import TESTDRIVER

__all__ = ["ListedTest", "ListedTestsError", "dumps_tests", "loads_tests"]


class ListedTestsError(Exception):
    """A list of tests cannot be read."""


@dataclass(frozen=True, slots=True)
class ListedTest:
    """One testharness.js test of the copy.

    Attributes:
        test: Its address from the root of web-platform-tests, with its query, such as
            "encoding/textdecoder-labels.any.worker.html" or "dom/nodes/Element-matches.html?xyz".
        long: Whether it has the long timeout, 60 seconds rather than 10.
        needs: What it needs that a Page cannot give in M8, by #260's scan, such as "layout".
    """

    test: str
    long: bool
    needs: list[str]

    @property
    def runs(self) -> bool:
        """Whether the harness runs it: a test that drives the browser through testdriver.js does not."""
        return TESTDRIVER not in self.needs

    @property
    def timeout(self) -> float:
        """How many seconds it has to finish, as wptrunner gives it."""
        return 60.0 if self.long else 10.0


def dumps_tests(tests: Iterable[ListedTest]) -> str:
    """Write a list of tests as text, one per line."""
    lines = [json.dumps(asdict(test)) for test in tests]
    return "[\n" + ",\n".join(lines) + "\n]\n" if lines else "[]\n"


def loads_tests(text: str) -> list[ListedTest]:
    """Read a list of tests.

    Raises:
        ListedTestsError: The text is not a list of tests.
    """
    try:
        entries = json.loads(text)
        if not isinstance(entries, list):
            raise TypeError("it is not a list")
        return [ListedTest(**entry) for entry in cast(list[dict[str, Any]], entries)]
    except (ValueError, TypeError) as error:
        raise ListedTestsError(f"not a list of tests: {error}") from error
