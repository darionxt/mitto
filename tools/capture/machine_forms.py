"""The form of each value that the normal form hides as <machine>, and a check that recordings keep to those forms.

A Parity Check cannot compare a value that the normal form hides, so its form is checked instead: how Chrome writes it,
which is how a Session writes it. Each form here is what Chrome on Windows can send for that hint, by docs/persona.md, "What
decides each value". The tests hold a Session's values to these forms, and this module's check holds real Chrome's.

The check reads the values of the machine that recorded them, so it runs where the recordings are kept, and it prints only
the forms, how many values each hint had, and whether they all had its form:

    uv run python -m tools.capture.machine_forms recordings/a recordings/b
"""

import argparse
import json
import re
import sys
from collections import Counter
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.capture.normal_form import HINTS_FROM_THE_MACHINE, MACHINE_PLACEHOLDER


@dataclass(frozen=True, slots=True)
class Form:
    """How the value of a hint is written.

    Attributes:
        description: The form in words. It says nothing of any one machine, so it can go anywhere.
        pattern: What a value of this form matches in full.
    """

    description: str
    pattern: re.Pattern[str]

    def fits(self, value: str) -> bool:
        """Return whether a value has this form."""
        return self.pattern.fullmatch(value) is not None


def _form(description: str, pattern: str) -> Form:
    return Form(description, re.compile(pattern))


_MEMORY = _form("2, 4, 8, 16 or 32", r"2|4|8|16|32")
_SCALE = _form("a number from 1 to below 10, in its shortest decimal, such as 1.25", r"[1-9](\.[0-9]*[1-9])?")
_SIZE = _form("a whole number from 1", r"[1-9][0-9]*")
_REDUCED = _form("no-preference or reduce", r"no-preference|reduce")

FORMS: Mapping[str, Form] = {
    "device-memory": _MEMORY,
    "sec-ch-device-memory": _MEMORY,
    "dpr": _SCALE,
    "sec-ch-dpr": _SCALE,
    "viewport-width": _SIZE,
    "sec-ch-viewport-width": _SIZE,
    "sec-ch-viewport-height": _SIZE,
    "rtt": _form("a multiple of 50 from 0 to 3000", r"0|50|[1-9][05]0|[12][0-9][05]0|3000"),
    "downlink": _form("a multiple of 0.05 from 0 to 10, in its shortest decimal", r"10|[0-9](\.([1-9]|[0-9]5))?"),
    "ect": _form("slow-2g, 2g, 3g or 4g", r"slow-2g|2g|3g|4g"),
    "sec-ch-prefers-color-scheme": _form("light or dark", r"light|dark"),
    "sec-ch-prefers-reduced-motion": _REDUCED,
    "sec-ch-prefers-reduced-transparency": _REDUCED,
    "sec-ch-ua-platform-version": _form(
        'the version of the UniversalApiContract of a Windows 10 or 11, in quotes, such as "19.0.0"',
        r'"(1|2|3|4|5|6|7|8|10|14|15|19)\.0\.0"',
    ),
}
"""The form of each hint that the normal form hides as <machine>, by lower-case name."""

_RAW_RECORDINGS = "*.raw.json"


@dataclass(frozen=True, slots=True)
class HintCount:
    """How the values of one hint kept to its form.

    Attributes:
        name: The hint, by lower-case name.
        values: How many values it had.
        misfits: How many of those did not have its form.
    """

    name: str
    values: int
    misfits: int


def hidden_values(raw: Mapping[str, Any]) -> Iterator[tuple[str, str]]:
    """Yield the lower-case name and the value of each line that the normal form hides, from a raw recording.

    Args:
        raw: A recording in the raw form that the capture kit writes, as JSON data.
    """
    for connection in raw["connections"]:
        for request in connection["requests"]:
            for line in request["headers"]:
                name, _, value = str(line).partition(": ")
                if name.lower() in HINTS_FROM_THE_MACHINE:
                    yield name.lower(), value


def count_forms(values: Iterable[tuple[str, str]]) -> list[HintCount]:
    """Count the values of each hint, and those without its form, in the order of the hints' names.

    Args:
        values: The lower-case name and the value of each line, as hidden_values() gives them.
    """
    seen: Counter[str] = Counter()
    misfits: Counter[str] = Counter()
    for name, value in values:
        seen[name] += 1
        if not FORMS[name].fits(value):
            misfits[name] += 1
    return [HintCount(name, seen[name], misfits[name]) for name in sorted(FORMS)]


def check_directory(directory: Path) -> tuple[int, list[HintCount]]:
    """Count the forms in every raw recording under a directory, such as one that record.ps1 wrote.

    Returns:
        How many raw recordings there are, and the count of each hint over all of them.
    """
    paths = sorted(directory.rglob(_RAW_RECORDINGS))
    values = (value for path in paths for value in hidden_values(json.loads(path.read_text(encoding="utf-8"))))
    return len(paths), count_forms(values)


def main(argv: Sequence[str] | None = None) -> int:
    """Check the directories that the command line names, print the forms and the outcome, and return the exit code."""
    parser = argparse.ArgumentParser(
        prog="python -m tools.capture.machine_forms",
        description=f"Check that each value a normal form hides as {MACHINE_PLACEHOLDER} has the form a Session's has.",
    )
    parser.add_argument("directories", nargs="+", type=Path, metavar="DIRECTORY", help="a directory of recordings")
    args = parser.parse_args(argv)
    passed = True
    for directory in args.directories:
        recordings, counts = check_directory(directory)
        values = sum(count.values for count in counts)
        directory_passed = values > 0 and not any(count.misfits for count in counts)
        passed = passed and directory_passed
        outcome = "pass" if directory_passed else "FAIL"
        print(f"{directory}: {outcome}, {values} values in {recordings} raw recordings")
        for count in counts:
            print(f"  {_outcome(count)}  {count.name}: {FORMS[count.name].description}, {_amount(count)}")
    return 0 if passed else 1


def _outcome(count: HintCount) -> str:
    if count.misfits:
        return "FAIL"
    return "pass" if count.values else "none"


def _amount(count: HintCount) -> str:
    if count.misfits:
        return f"{count.misfits} of {count.values} values without it"
    return f"{count.values} values"


if __name__ == "__main__":
    sys.exit(main())
