"""Compares the Chromium files that mitto ports between two releases.

    MITTO_WORKSPACE=/absolute/path uv run python -m tools.workspace.upstream <old-tag> <new-tag>

Some of mitto is ported from Chromium rather than built from it. A file in native/ or src/mitto/ that ports code names
each file it comes from on a line of its own, beside its prose: `// Upstream: <path from src>` in C++ and
`# Upstream: <path from src>` in Python and GN. This collects those paths, runs tools/workspace/compare on them, and
then prints the diff of each one between the two tags, so that a run of the release routine sees every change that a
port may have to follow. It fails if a path is missing at either tag, and before it compares anything if a file that
says it is ported names no upstream file, or if a line meant as an Upstream line is not in that form.

Progress and warnings go to stderr, and the comparison and the diffs to stdout. See docs/releasing.md.
"""

import argparse
import os
import re
import subprocess
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

__all__ = [
    "PORT_DIRECTORIES",
    "REPO",
    "PortFile",
    "form_problems",
    "missing_paths",
    "port_files",
    "print_diffs",
    "read_port_file",
    "upstream_paths",
]

REPO = Path(__file__).resolve().parents[2]

# Where code that ports from Chromium lives, relative to the root of the repository.
PORT_DIRECTORIES = ("native", "src/mitto")

COMPARE = REPO / "tools" / "workspace" / "compare"

# What starts a comment, for each kind of file that can port code.
_COMMENT_MARKERS = {".c": "//", ".cc": "//", ".h": "//", ".mm": "//", ".gn": "#", ".gni": "#", ".py": "#"}

_UPSTREAM_PREFIX = "Upstream: "

# A comment that is meant as an Upstream line, in whatever form.
_MEANT_AS_UPSTREAM = re.compile(r"^upstream\s*:", re.IGNORECASE)

# A comment that says its file is ported, or is a port, of something.
_SAYS_PORTED = re.compile(r"\bported\b|\ba port (?:of|from)\b", re.IGNORECASE)

# A path in Chromium's src: relative, with no empty, '.' or '..' part.
_PATH_PART = re.compile(r"^[A-Za-z0-9_.+-]+$")


@dataclass(frozen=True, slots=True)
class PortFile:
    """What a file in the repository says about where its code comes from.

    Attributes:
        path: The file, relative to the root of the repository.
        says_ported: Whether a comment of it says that it is ported, or a port.
        upstream: Each path from Chromium's src that an Upstream line names, in the order of the lines.
        malformed: Each line that is meant as an Upstream line but is not in the fixed form, as `number: line`.
    """

    path: PurePosixPath
    says_ported: bool
    upstream: tuple[str, ...]
    malformed: tuple[str, ...]


def _is_path_from_src(path: str) -> bool:
    parts = path.split("/")
    return all(_PATH_PART.match(part) and part not in (".", "..") for part in parts)


def read_port_file(path: Path, root: Path = REPO) -> PortFile | None:
    """Read what `path` says about where its code comes from, or None if it is not a kind of file that can port code.

    Only whole-line comments count, so that neither code nor a comment after code can look like an Upstream line.
    """
    marker = _COMMENT_MARKERS.get(path.suffix)
    if marker is None:
        return None
    says_ported = False
    upstream: list[str] = []
    malformed: list[str] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.lstrip(" ")
        if not stripped.startswith(marker):
            continue
        comment = stripped.removeprefix(marker)
        if _SAYS_PORTED.search(comment):
            says_ported = True
        if not _MEANT_AS_UPSTREAM.match(comment.strip()):
            continue
        candidate = comment.removeprefix(" " + _UPSTREAM_PREFIX)
        if comment.startswith(" " + _UPSTREAM_PREFIX) and _is_path_from_src(candidate):
            upstream.append(candidate)
        else:
            malformed.append(f"{number}: {line}")
    return PortFile(
        path=PurePosixPath(path.relative_to(root).as_posix()),
        says_ported=says_ported,
        upstream=tuple(upstream),
        malformed=tuple(malformed),
    )


def port_files(root: Path = REPO) -> list[PortFile]:
    """Read every file under PORT_DIRECTORIES that can port code, in the order of their paths."""
    files: list[PortFile] = []
    for directory in PORT_DIRECTORIES:
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and (port_file := read_port_file(path, root)) is not None:
                files.append(port_file)
    return files


def form_problems(files: Iterable[PortFile]) -> list[str]:
    """Return what is wrong with how `files` name their upstream files, one line each, or nothing."""
    problems: list[str] = []
    for port_file in files:
        if port_file.says_ported and not port_file.upstream:
            problems.append(f"{port_file.path} says it is ported, but has no line '{_UPSTREAM_PREFIX}<path from src>'")
        problems.extend(
            f"{port_file.path}:{line} is not '{_UPSTREAM_PREFIX}<path from src>'" for line in port_file.malformed
        )
    return problems


def upstream_paths(files: Iterable[PortFile]) -> dict[str, list[PurePosixPath]]:
    """Return each upstream path that `files` name, in order, with the files that name it."""
    named: dict[str, list[PurePosixPath]] = {}
    for port_file in files:
        for path in port_file.upstream:
            ports = named.setdefault(path, [])
            if port_file.path not in ports:
                ports.append(port_file.path)
    return dict(sorted(named.items()))


def _git(repository: Path, *args: str) -> subprocess.CompletedProcess[str]:
    # Every path is a path, never a pattern.
    env = {**os.environ, "GIT_LITERAL_PATHSPECS": "1"}
    return subprocess.run(
        ["git", "-C", str(repository), *args],
        env=env,
        check=True,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
    )


def missing_paths(repository: Path, tag: str, paths: Sequence[str]) -> list[str]:
    """Return those of `paths` that are not a file at `tag` in `repository`, in order."""
    listed = _git(repository, "ls-tree", "-r", "-z", "--name-only", tag, "--", *paths).stdout
    files = set(listed.split("\0"))
    return [path for path in paths if path not in files]


def print_diffs(repository: Path, old_tag: str, new_tag: str, named: Mapping[str, Sequence[PurePosixPath]]) -> None:
    """Print the diff of each upstream path in `named` from `old_tag` to `new_tag`, with the files that port it."""
    changed = 0
    for path, ports in named.items():
        print(f"== {path}, ported in {', '.join(str(port) for port in ports)}")
        diff = _git(repository, "diff", "--no-color", "--no-ext-diff", old_tag, new_tag, "--", path).stdout
        if diff:
            changed += 1
            print(diff, end="")
        else:
            print(" no changes")
        print()
    print(f"{len(named)} upstream files from {old_tag} to {new_tag}: {changed} changed, {len(named) - changed} not.")


def _compare_repository() -> Path:
    # Where tools/workspace/compare keeps its shallow repository of chromium/src, once it has checked MITTO_WORKSPACE.
    return Path(os.environ["MITTO_WORKSPACE"]).resolve() / "compare" / "src"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old_tag", help="the Chromium version that the ports follow now, such as 156.0.8078.80")
    parser.add_argument("new_tag", help="the Chromium version to compare it with, such as 157.0.8103.39")
    arguments = parser.parse_args(argv)

    files = port_files()
    if problems := form_problems(files):
        for problem in problems:
            print(f"upstream: error: {problem}", file=sys.stderr)
        return 1
    named = upstream_paths(files)
    paths = list(named)

    # compare fetches both tags, and checks MITTO_WORKSPACE.
    sys.stdout.flush()
    compared = subprocess.run([str(COMPARE), arguments.old_tag, arguments.new_tag, *paths], check=False)
    if compared.returncode != 0:
        return compared.returncode

    repository = _compare_repository()
    missing = {tag: missing_paths(repository, tag, paths) for tag in (arguments.old_tag, arguments.new_tag)}
    print_diffs(repository, arguments.old_tag, arguments.new_tag, named)
    sys.stdout.flush()
    for tag, tag_missing in missing.items():
        for path in tag_missing:
            ports = ", ".join(str(port) for port in named[path])
            print(f"upstream: error: {tag} has no file {path}, which {ports} names", file=sys.stderr)
    return 1 if any(missing.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
