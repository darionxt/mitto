"""Dropping the oldest Chrome Release from the Supported Window, once the new Engine Release has its Persona.

What goes is its Persona's file, its Baselines, its entries in the known-gaps list and its name in
src/mitto/_persona.py. The docs are left to whoever runs it, by the rule in docs/releasing.md. Everything is checked
before anything changes, so a refusal leaves the tree as it was.
"""

import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from tools.parity import KNOWN_GAPS_FILE_NAME, dumps_known_gaps, loads_known_gaps
from tools.release.persona import persona_path
from tools.release.window import WINDOW_SIZE

__all__ = ["REPOSITORY", "DropError", "Dropped", "Named", "drop_oldest", "releases", "still_named"]

REPOSITORY = Path(__file__).parents[2]

_PERSONAS = Path("src/mitto/personas")
_PERSONA_MODULE = Path("src/mitto/_persona.py")
_BASELINES = Path("tests/baselines")
_PERSONA_FILE = re.compile(r"chrome_([0-9]+)_windows\.json")

# Where code lives that could name a Chrome Release, and what naming one looks like there.
_CODE_DIRECTORIES = ("src", "tests", "tools")
_CODE_SUFFIXES = (".py", ".ps1")


class DropError(Exception):
    """The oldest Chrome Release cannot be dropped, and nothing has changed."""


@dataclass(frozen=True, slots=True)
class Named:
    """A line of code that names a Chrome Release.

    Attributes:
        path: The file, relative to the root of the tree.
        number: The line's number, from 1.
        line: The line, without its end.
    """

    path: Path
    number: int
    line: str

    def __str__(self) -> str:
        return f"{self.path.as_posix()}:{self.number}: {self.line.strip()}"


@dataclass(frozen=True, slots=True)
class Dropped:
    """What drop_oldest() did.

    Attributes:
        release: The Chrome Release it dropped.
        gaps: How many entries of the known-gaps list it took out.
        still_named: Lines of code that still name the Chrome Release, which it did not know how to change.
    """

    release: str
    gaps: int
    still_named: list[Named]


def releases(root: Path = REPOSITORY) -> list[str]:
    """Return the Chrome Releases that have a Persona's file in the tree, oldest first."""
    found = [match[1] for path in (root / _PERSONAS).iterdir() if (match := _PERSONA_FILE.fullmatch(path.name))]
    return sorted(found, key=int)


def drop_oldest(root: Path = REPOSITORY) -> Dropped:
    """Drop the oldest Chrome Release from the tree at root.

    Raises:
        DropError: The Supported Window would hold fewer than three Chrome Releases, the oldest has no Baselines, the
            known-gaps list cannot be read, or src/mitto/_persona.py names it where this does not know how to change it.
    """
    window = releases(root)
    if len(window) <= WINDOW_SIZE:
        raise DropError(
            f"the Personas are of Chrome {', '.join(window)}, and the Supported Window holds {WINDOW_SIZE}. "
            "Write the Persona of the new Engine Release first."
        )
    oldest = window[0]
    baselines = root / _BASELINES / oldest
    if not baselines.is_dir():
        raise DropError(f"Chrome {oldest} has no Baselines in {baselines}")
    module = root / _PERSONA_MODULE
    module_text = _without(module.read_text(encoding="utf-8"), oldest)
    known_gaps = root / _BASELINES / KNOWN_GAPS_FILE_NAME
    gaps = loads_known_gaps(known_gaps.read_text(encoding="utf-8")) if known_gaps.exists() else []
    kept = [gap for gap in gaps if gap.release != oldest]

    persona_path(oldest, root / _PERSONAS).unlink()
    shutil.rmtree(baselines)
    if known_gaps.exists():
        known_gaps.write_text(dumps_known_gaps(kept), encoding="utf-8")
    module.write_text(module_text, encoding="utf-8")
    return Dropped(oldest, len(gaps) - len(kept), still_named(oldest, root))


def still_named(release: str, root: Path = REPOSITORY) -> list[Named]:
    """Return the lines of code in the tree that name a Chrome Release: its Persona, one of its builds or its User-Agent.

    Baselines and other data are not code, and neither is the number alone, which tests also use as a label.
    """
    naming = re.compile(rf"chrome_{release}_windows|\b{release}\.0\.[0-9]+\.[0-9]+|Chrome/{release}\.", re.IGNORECASE)
    found: list[Named] = []
    for directory in _CODE_DIRECTORIES:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in _CODE_SUFFIXES or not path.is_file():
                continue
            lines = path.read_text(encoding="utf-8").splitlines()
            found += [
                Named(path.relative_to(root), number, line)
                for number, line in enumerate(lines, start=1)
                if naming.search(line)
            ]
    return found


def _without(module_text: str, release: str) -> str:
    """Return the text of src/mitto/_persona.py without the name of a Chrome Release's Persona.

    Raises:
        DropError: The text names the Persona anywhere else, such as as the Engine Release's.
    """
    constant = f"CHROME_{release}_WINDOWS"
    text = module_text
    for line in (
        # In __all__, as the name of its file, and in WINDOWS_PERSONAS, on one line or on a line of its own.
        rf'^    "{constant}",\n',
        rf'^{constant} = "chrome_{release}_windows"\n',
        rf'(?:^    )?"{release}": {constant},(?:\n| )',
    ):
        pattern = re.compile(line, re.MULTILINE)
        if len(pattern.findall(text)) != 1:
            raise DropError(f"{_PERSONA_MODULE} does not hold {pattern.pattern!r} once, so change it by hand")
        text = pattern.sub("", text)
    if constant in text:
        raise DropError(
            f"{_PERSONA_MODULE} still names {constant} after taking it out of the list, so change it by hand"
        )
    return text
