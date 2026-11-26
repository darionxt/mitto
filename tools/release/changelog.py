"""CHANGELOG.md, and the files of changelog.d that the release's own pull request gathers into it. See docs/releasing.md.

CHANGELOG.md says what changes for callers in each version, under a heading `## X.Y.Z`, newest first. A pull request whose
change callers will notice writes its lines in a file of its own, `changelog.d/<branch>.md`, rather than in CHANGELOG.md,
so that two pull requests never change the same lines. The release's pull request gathers the files into its version's
entry, in the order that they came to main, and deletes them.
"""

import re
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

__all__ = [
    "CHANGELOG",
    "CHANGES_DIRECTORY",
    "REPOSITORY",
    "ChangelogError",
    "Entry",
    "change_files",
    "entries",
    "gather",
    "in_arrival_order",
    "lines_of",
    "with_lines",
]

REPOSITORY = Path(__file__).parents[2]

CHANGELOG = Path("CHANGELOG.md")
CHANGES_DIRECTORY = Path("changelog.d")
"""Where each pull request writes its lines for callers, in a file of its own."""

_NUMBER = r"(?:0|[1-9][0-9]*)"
_RELEASE = re.compile(rf"{_NUMBER}\.{_NUMBER}\.{_NUMBER}")
# A heading of CHANGELOG.md, which starts the entry of the version it names.
_ENTRY_HEADING = re.compile(r"(?m)^## (.*)$")
# An item of a list, one a line, as each line of an entry's list is.
_ITEM = re.compile(r"- \S.*")


class ChangelogError(Exception):
    """CHANGELOG.md or a file of changelog.d is not as the release routine writes it, so nothing was gathered."""


@dataclass(frozen=True, slots=True)
class Entry:
    """The entry of one version in the text of CHANGELOG.md.

    Attributes:
        version: The version that its heading names, such as 0.1.0.
        heading: Where its heading starts.
        start: Where its text starts, right after the heading.
        end: Where its text ends, at the next heading or at the end of the file.
    """

    version: str
    heading: int
    start: int
    end: int


def entries(text: str) -> list[Entry]:
    """Return the entries in the text of CHANGELOG.md, in the order of the file, which is newest first.

    The text before the first heading says what the file is, and belongs to no version.

    Raises:
        ChangelogError: A heading is not a version, such as 0.1.0, or names a version that another heading names.
    """
    headings = list(_ENTRY_HEADING.finditer(text))
    ends = [*(heading.start() for heading in headings[1:]), len(text)] if headings else []
    found: list[Entry] = []
    for heading, end in zip(headings, ends, strict=True):
        version = heading[1]
        if _RELEASE.fullmatch(version) is None:
            raise ChangelogError(f"{CHANGELOG} has a heading {version!r}, where each names a version, such as 0.1.0")
        if any(entry.version == version for entry in found):
            raise ChangelogError(f"{CHANGELOG} has two entries for {version}")
        found.append(Entry(version, heading.start(), heading.end(), end))
    return found


def with_lines(text: str, version: str, lines: Sequence[str]) -> str:
    """Return the text of CHANGELOG.md with these lines at the end of a version's entry, as a hand edit would write them.

    They continue the entry's list, or start one after its last paragraph. A version with no entry gets one, above the
    newest version before it, so that the entries stay newest first.

    Raises:
        ChangelogError: The version is not one, such as 0.1.0, or the text's headings are not as entries() reads them.
    """
    if _RELEASE.fullmatch(version) is None:
        raise ChangelogError(f"{version} is not the version of a release, such as 0.1.0")
    found = entries(text)
    if not lines:
        return text
    added = "\n".join(lines)
    if (entry := next((entry for entry in found if entry.version == version), None)) is not None:
        before = text[entry.start : entry.end].strip()
        if not before:
            body = added
        elif _ITEM.fullmatch(before.rsplit("\n", 1)[-1]):
            body = f"{before}\n{added}"
        else:
            body = f"{before}\n\n{added}"
        after = "\n\n" if entry.end < len(text) else "\n"
        return f"{text[: entry.start]}\n\n{body}{after}{text[entry.end :]}"
    new_entry = f"## {version}\n\n{added}"
    older = next((entry for entry in found if _key(entry.version) < _key(version)), None)
    if older is not None:
        return f"{text[: older.heading]}{new_entry}\n\n{text[older.heading :]}"
    if text.strip():
        return f"{text.rstrip()}\n\n{new_entry}\n"
    return f"{new_entry}\n"


def change_files(root: Path = REPOSITORY) -> list[Path]:
    """Return what changelog.d holds, relative to the root, by name, or nothing if there is no such directory.

    A name that starts with a dot, such as macOS's .DS_Store, is no change's.
    """
    directory = root / CHANGES_DIRECTORY
    if not directory.is_dir():
        return []
    return sorted(path.relative_to(root) for path in directory.iterdir() if not path.name.startswith("."))


def lines_of(path: Path, root: Path = REPOSITORY) -> list[str]:
    """Return the lines that a file of changelog.d, relative to the root, gives its version's entry: each an item of a
    list, as in the entry.

    Blank lines and the spaces at the end of a line are left out.

    Raises:
        ChangelogError: The path is not a file named `<branch>.md`, or the file holds no item, or a line that is none.
    """
    name = path.as_posix()
    if path.suffix != ".md" or not (root / path).is_file():
        raise ChangelogError(f"{name} is not a file of lines for callers, which is named after its branch, as name.md")
    lines: list[str] = []
    for number, line in enumerate((root / path).read_text(encoding="utf-8").splitlines(), start=1):
        if not (line := line.rstrip()):
            continue
        if _ITEM.fullmatch(line) is None:
            raise ChangelogError(f'{name}:{number} is not an item of a list, a line that starts with "- "')
        lines.append(line)
    if not lines:
        raise ChangelogError(f"{name} holds no line for callers")
    return lines


def in_arrival_order(files: Sequence[Path], root: Path = REPOSITORY) -> list[Path]:
    """Return files of changelog.d in the order that commits added them, oldest first, which is the order that squash
    merges brought them to main.

    The files are relative to the root, which is the top of a git repository. A file added again after it went counts
    from the last time. A file that no commit has added comes after the others, and files that one commit added, or
    that none did, come by name.

    Raises:
        ChangelogError: git cannot read the root's history.
    """
    # Each commit's names come after a mark of their own, each name ended by a NUL.
    command = ["git", "-C", str(root), "log", "--no-renames", "--diff-filter=A", "--name-only", "--format=%x01", "-z"]
    try:
        log = subprocess.run(
            [*command, "--", CHANGES_DIRECTORY.as_posix()],
            check=True,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
        raise ChangelogError(f"git cannot read when the files of {CHANGES_DIRECTORY} came: {detail}") from error
    # Newest commit first, so a file's first place is the last commit that added it.
    added: dict[str, int] = {}
    for place, commit in enumerate(log.split("\x01")[1:]):
        for name in commit.split("\0"):
            if name := name.removeprefix("\n"):
                added.setdefault(name, place)

    def key(path: Path) -> tuple[bool, int, str]:
        place = added.get(path.as_posix())
        return (place is None, -(place or 0), path.as_posix())

    return sorted(files, key=key)


def gather(version: str, root: Path = REPOSITORY) -> list[Path]:
    """Write the lines of changelog.d's files at the end of a version's entry in CHANGELOG.md, and delete the files.

    The files come in the order that they came to main, each with its lines in their order. With no file, nothing
    changes. The root is the top of a git repository.

    Returns:
        The files gathered, relative to the root, in that order.

    Raises:
        ChangelogError: The version is not one, such as 0.1.0, a file is not one of lines for callers, git cannot read
            the history, or CHANGELOG.md's headings are not each one version once. Nothing changes then.
    """
    if _RELEASE.fullmatch(version) is None:
        raise ChangelogError(f"{version} is not the version of a release, such as 0.1.0")
    files = change_files(root)
    if not files:
        return []
    lines_by_file = {path: lines_of(path, root) for path in files}
    files = in_arrival_order(files, root)
    lines = [line for path in files for line in lines_by_file[path]]
    changelog = root / CHANGELOG
    changelog.write_text(
        with_lines(changelog.read_text(encoding="utf-8"), version, lines), encoding="utf-8", newline="\n"
    )
    for path in files:
        (root / path).unlink()
    return files


def _key(version: str) -> tuple[int, ...]:
    return tuple(int(number) for number in version.split("."))
