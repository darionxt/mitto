"""Writes THIRD_PARTY_NOTICES: the licences of every project linked into the native library.

A wheel holds the native library, which links Chromium and many projects that Chromium uses, and their licences ask
that a binary carries their notices. Which projects these are comes from the library's build graph: what the shared
library links, through static libraries, source sets, Rust libraries and groups. A code generator or another build
tool that only runs during the build is not linked, so it needs no notice. Each project is named by the README.chromium
of its directory, as Chromium's tools/licenses/licenses.py does for Chrome.

The Hosts link slightly different projects, and gn on one Host cannot describe another's build, so each entry says
which Hosts link its project, and each Host has its own part:

    uv run python -m tools.workspace.notices update /path/to/chromium-<tag>/src/out/mitto
    uv run python -m tools.workspace.notices host /path/to/chromium-<tag>/src/out/mitto > part
    uv run python -m tools.workspace.notices merge part

update rewrites this Host's part of THIRD_PARTY_NOTICES from a build directory of the source export or of a checkout.
host prints only this Host's part, which a test compares with a wheel's notices, and merge takes such a part from
another Host, such as one that CI made. See docs/building.md.
"""

import argparse
import dataclasses
import json
import posixpath
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import Any, cast

from tools.workspace import export

NOTICES_FILE = export.REPO / "THIRD_PARTY_NOTICES"

ROOT_TARGET = "//mitto:mitto_core"

# What a shared library links of a dependency: its code, or for a group, its own dependencies.
LINKED_TYPES = {"group", "rust_library", "source_set", "static_library"}
CODE_TYPES = {"rust_library", "shared_library", "source_set", "static_library"}

HOST_NAMES = {export.LINUX.name: "Linux x86_64", export.MAC.name: "macOS arm64", export.WINDOWS.name: "Windows x64"}

# A third-party project lies in a directory under third_party, and a Rust crate in third_party/rust/<crate>/<epoch>. The
# last such directory in a path is the project, so that a project inside another one counts on its own.
PROJECT_DIRECTORY = re.compile(r"^((?:.+/)?third_party/(?:rust/[^/]+/v[^/]+|[^/]+))(?:/|$)")

# Where the sources of each Rust crate lie. Its README.chromium is in the directory of its target instead.
RUST_CRATE_SOURCES = "third_party/rust/chromium_crates_io/"

# The Rust standard library, which the build compiles from the sources in the Rust toolchain.
RUST_LIBRARY = "third_party/rust-toolchain/lib/rustlib/src/rust/library"
RUST_LICENCES = "//third_party/rust-toolchain/share/doc/rustc/licenses"

LICENCE_NAME = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)([._-].*)?$", re.IGNORECASE)
README_CHROMIUM = "README.chromium"
DEPENDENCY_DIVIDER = re.compile(r"^-{20} DEPENDENCY DIVIDER -{20}$")
NOT_SHIPPED = "NOT_SHIPPED"

SEPARATOR = "=" * 80

HEADER = """\
Third-party notices for mitto's native library

Each wheel of mitto holds a native library, mitto/libmitto_core.so on Linux,
mitto/libmitto_core.dylib on macOS and mitto/mitto_core.dll on Windows, built
from Chromium's network stack. mitto's own code is under the licence in LICENSE.
The entries below are the projects linked into the library: Chromium itself and
the projects it uses, each with the text of its licence.

"Directory" is where a project lies in Chromium's source tree, and "Linked on"
names the Hosts whose library links it.

tools/workspace/notices.py writes this file from the library's build graph. See
docs/building.md.
"""


class NoticesError(Exception):
    """Stops the script with a message for the person running it."""


@dataclasses.dataclass(frozen=True)
class Dependency:
    """What a README.chromium says of one dependency: its name, where it comes from and which files hold its licence.

    A licence file is named from the top of the source tree, as in GN, such as //LICENSE.
    """

    name: str
    url: str
    licence: str
    licence_files: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class Entry:
    """One project's notice, as THIRD_PARTY_NOTICES holds it.

    The body is everything below the lines that name the entry and its Hosts: where the project comes from and the text
    of each of its licence files.
    """

    name: str
    directory: str
    hosts: frozenset[str]
    body: str

    @property
    def key(self) -> tuple[str, str]:
        return self.directory, self.name


# Directories whose licence no README.chromium names.
SPECIAL_CASES = {
    "": Dependency("The Chromium Project", "https://www.chromium.org", "BSD-3-Clause", ("//LICENSE",)),
    RUST_LIBRARY: Dependency(
        "The Rust Standard Library",
        "https://www.rust-lang.org",
        "Apache-2.0 OR MIT, by the Rust Project Developers; Unicode-3.0 for the Unicode tables in core",
        (f"{RUST_LICENCES}/Apache-2.0.txt", f"{RUST_LICENCES}/MIT.txt", f"{RUST_LICENCES}/Unicode-3.0.txt"),
    ),
}


# Reading the build graph.


def describe(src: Path, build_dir: Path) -> dict[str, Any]:
    """Returns what gn knows of every target in the build graph of a build directory.

    gn runs in a directory of its own with the same arguments, because evaluating the build files writes into the
    build directory, and the build's own files must stay as they are.
    """
    host = export.current_host()
    with tempfile.TemporaryDirectory(dir=src / "out", prefix=".notices-") as temporary:
        directory = Path(temporary)
        (directory / "args.gn").write_text((build_dir / "args.gn").read_text())
        # gn desc needs a build.ninja but not its contents.
        (directory / "build.ninja").touch()
        command = [src / host.gn, "desc", directory, "//*", "--format=json", *export.GN_ROOT]
        result = subprocess.run(
            command, cwd=src, env=export.build_environment(), capture_output=True, encoding="utf-8", check=False
        )
    if result.returncode != 0:
        raise NoticesError(f"gn desc failed with status {result.returncode}:\n{result.stdout}{result.stderr}")
    return json.loads(result.stdout)


def linked_targets(graph: Mapping[str, Any]) -> list[str]:
    """Returns the labels of the library and of every target linked into it."""
    if ROOT_TARGET not in graph:
        raise NoticesError(f"the build graph has no {ROOT_TARGET}")
    linked: set[str] = set()
    pending = [ROOT_TARGET]
    while pending:
        label = pending.pop()
        if label in linked:
            continue
        linked.add(label)
        pending.extend(dep for dep in graph[label].get("deps", []) if graph[dep]["type"] in LINKED_TYPES)
    return sorted(linked)


def code_directories(label: str, target: Mapping[str, Any]) -> set[str]:
    """Returns the directories of the source tree whose code a target compiles.

    These are the directories of its sources, or of the target itself when all its sources are generated. A Rust
    crate's code counts as its target's directory, which holds its README.chromium. The library's own code is mitto's.
    """
    own_directory = label.removeprefix("//").partition(":")[0]
    directories: set[str] = set()
    for key in ("sources", "public"):
        files = target.get(key, [])
        if not isinstance(files, list):
            continue
        for file in cast("list[str]", files):
            path = file.removeprefix("//")
            # Generated files lie in the build directory, and files outside the source tree have absolute paths.
            if not file.startswith("//") or path.startswith(("out/", "mitto/")):
                continue
            directories.add(own_directory if path.startswith(RUST_CRATE_SOURCES) else str(PurePosixPath(path).parent))
    if label == ROOT_TARGET:
        return directories
    return directories or {own_directory}


def linked_code_directories(graph: Mapping[str, Any]) -> set[str]:
    directories: set[str] = set()
    for label in linked_targets(graph):
        if graph[label]["type"] in CODE_TYPES:
            directories |= code_directories(label, graph[label])
    return directories


# Finding the project of a directory and its licence.


def project_of(src: Path, directory: str) -> str:
    """Returns the project that the code in a directory belongs to, as a directory; Chromium's own is "".

    A project's directory holds its README.chromium. One without, such as a project inside another project, is the
    nearest directory that holds licence files, and one with neither belongs to the project around it.
    """
    match = PROJECT_DIRECTORY.match(directory)
    if match is None:
        return ""
    project = match[1]
    if (src / project / README_CHROMIUM).is_file():
        return project
    path = PurePosixPath(directory)
    for candidate in (path, *path.parents):
        name = str(candidate)
        if name in SPECIAL_CASES or _licence_files_in(src, name):
            return name
        if name == project:
            break
    return project_of(src, str(PurePosixPath(project).parent))


def _licence_files_in(src: Path, directory: str) -> list[str]:
    folder = src / directory
    if not folder.is_dir():
        return []
    return sorted(
        f"//{directory}/{file.name}" for file in folder.iterdir() if file.is_file() and LICENCE_NAME.match(file.name)
    )


def dependencies_of(src: Path, project: str) -> list[Dependency]:
    """Returns the dependencies that a project's directory ships, with their licence files.

    Raises:
        NoticesError: If a dependency has no licence file.
    """
    if project in SPECIAL_CASES:
        dependency = SPECIAL_CASES[project]
        missing = [path for path in dependency.licence_files if not (src / path.removeprefix("//")).is_file()]
        if missing:
            raise NoticesError(f"{', '.join(missing)} is missing, which holds the licence of {dependency.name}")
        return [dependency]
    readme = src / project / README_CHROMIUM
    if not readme.is_file():
        if not (files := _licence_files_in(src, project)):
            raise NoticesError(f"//{project} has neither a {README_CHROMIUM} nor a licence file")
        return [Dependency(PurePosixPath(project).name, "", "", tuple(files))]
    dependencies: list[Dependency] = []
    for fields in parse_readme(readme.read_text(encoding="utf-8", errors="replace")):
        files = [file.strip() for file in fields.get("license file", "LICENSE").split(",")]
        shipped = fields.get("shipped in chromium", fields.get("shipped", "no" if NOT_SHIPPED in files else "yes"))
        if shipped.lower() == "no":
            continue
        name = fields.get("name", PurePosixPath(project).name)
        licence_files = _resolve(src, project, files) or _resolve(src, project, ["COPYING"])
        if not licence_files:
            raise NoticesError(f"{name} in //{project} names no licence file that exists; see its {README_CHROMIUM}")
        dependencies.append(Dependency(name, fields.get("url", ""), fields.get("license", ""), tuple(licence_files)))
    return dependencies


def parse_readme(text: str) -> list[dict[str, str]]:
    """Returns the fields of each dependency that a README.chromium describes, by lower-case name.

    Raises:
        NoticesError: If a dependency names a field twice.
    """
    dependencies: list[dict[str, str]] = []
    fields: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if DEPENDENCY_DIVIDER.match(line):
            if fields:
                dependencies.append(fields)
            fields = {}
            continue
        name, separator, value = line.partition(": ")
        name = name.lower()
        if not separator or name not in {"name", "url", "license", "license file", "shipped", "shipped in chromium"}:
            continue
        if name in fields:
            raise NoticesError(f"a {README_CHROMIUM} names '{name}' twice for one dependency")
        fields[name] = value.strip()
    if fields:
        dependencies.append(fields)
    return dependencies


def _resolve(src: Path, project: str, files: Iterable[str]) -> list[str]:
    """Returns the licence files that a README.chromium names and that exist, from the top of the source tree."""
    resolved: list[str] = []
    for file in files:
        if not file or file == NOT_SHIPPED:
            continue
        # A leading slash means the top of the source tree, as in GN, whose labels use slashes on every Host.
        path = posixpath.normpath(file.lstrip("/") if file.startswith("/") else f"{project}/{file}")
        if not path.startswith("..") and (src / path).is_file():
            resolved.append(f"//{path}")
    return resolved


# Making the entries.


def host_entries(src: Path, build_dir: Path) -> list[Entry]:
    """Returns the entry of every project linked into the library of a build directory, linked on this Host."""
    host = export.current_host().name
    projects = {project_of(src, directory) for directory in linked_code_directories(describe(src, build_dir))}
    entries: list[Entry] = []
    for project in projects:
        for dependency in dependencies_of(src, project):
            entries.append(Entry(dependency.name, f"//{project}", frozenset({host}), entry_body(src, dependency)))
    return entries


def entry_body(src: Path, dependency: Dependency) -> str:
    lines = [f"URL: {dependency.url}"] if dependency.url else []
    if dependency.licence:
        lines.append(f"Licence: {dependency.licence}")
    for file in dependency.licence_files:
        lines += ["", f"Licence file: {file}", "", licence_text(src / file.removeprefix("//"))]
    return "\n".join(lines) + "\n"


def licence_text(file: Path) -> str:
    """Returns the text of a licence file, with Unix line ends and no trailing white space.

    Raises:
        NoticesError: If a line of the text could be taken for the line between two entries.
    """
    text = file.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.rstrip() for line in text.split("\n")]
    if SEPARATOR in lines:
        raise NoticesError(f"{file} holds a line that separates two entries of the notices")
    return "\n".join(lines).strip("\n")


# Reading, merging and writing the notices.


def render(entries: Iterable[Entry]) -> str:
    """Returns the text of a notices file with these entries: Chromium first, then by name."""
    blocks = [HEADER]
    for entry in sorted(entries, key=lambda entry: (entry.directory != "//", entry.name.lower(), entry.directory)):
        hosts = ", ".join(HOST_NAMES[host] for host in sorted(entry.hosts))
        blocks.append(
            f"{SEPARATOR}\nName: {entry.name}\nDirectory: {entry.directory}\nLinked on: {hosts}\n{entry.body}"
        )
    return "\n".join(blocks)


def parse(text: str) -> list[Entry]:
    """Returns the entries of a notices file that render() wrote.

    Raises:
        NoticesError: If the text is not such a file.
    """
    header, *blocks = text.split(f"\n{SEPARATOR}\n")
    if header != HEADER:
        raise NoticesError("the notices do not start with the header that tools/workspace/notices.py writes")
    hosts_by_name = {name: host for host, name in HOST_NAMES.items()}
    entries: list[Entry] = []
    for block in blocks:
        match = re.match(r"Name: (.+)\nDirectory: (//.*)\nLinked on: (.+)\n", block)
        if match is None:
            raise NoticesError(
                f"an entry of the notices does not start with its name, directory and Hosts:\n{block[:200]}"
            )
        names = match[3].split(", ")
        if not set(names) <= hosts_by_name.keys():
            raise NoticesError(
                f"the entry of {match[1]} names a Host that is not one of {', '.join(HOST_NAMES.values())}"
            )
        body = block[match.end() :]
        entries.append(
            Entry(match[1], match[2], frozenset(hosts_by_name[name] for name in names), body.removesuffix("\n") + "\n")
        )
    return entries


def merge(entries: Sequence[Entry], host: str, fresh: Sequence[Entry]) -> list[Entry]:
    """Returns the entries with one Host's part replaced: those of the projects that its library links now.

    An entry that no Host links any longer goes. One that both link takes its text from the fresh entries.
    """
    merged: dict[tuple[str, str], Entry] = {}
    for entry in entries:
        if hosts := entry.hosts - {host}:
            merged[entry.key] = dataclasses.replace(entry, hosts=hosts)
    for entry in fresh:
        others = merged[entry.key].hosts if entry.key in merged else frozenset[str]()
        merged[entry.key] = dataclasses.replace(entry, hosts=others | {host})
    return list(merged.values())


def differences(entries: Sequence[Entry], host: str, fresh: Sequence[Entry]) -> list[str]:
    """Says where the entries differ from what one Host's library links now, one line per project."""
    name = HOST_NAMES[host]
    linked = {entry.key: entry for entry in entries if host in entry.hosts}
    wanted = {entry.key: entry for entry in fresh}
    lines = [
        f"no notice for {key[1]} ({key[0]}), which the library links on {name}" for key in wanted.keys() - linked.keys()
    ]
    lines += [
        f"{key[1]} ({key[0]}) is not linked on {name}, but its entry says so" for key in linked.keys() - wanted.keys()
    ]
    lines += [
        f"the notice for {key[1]} ({key[0]}) differs from its licence files"
        for key in wanted.keys() & linked.keys()
        if wanted[key].body != linked[key].body
    ]
    return sorted(lines)


def source_tree_of(build_dir: Path) -> Path:
    src = build_dir.resolve().parent.parent
    if not (build_dir / "args.gn").is_file() or not (src / ".gn").is_file():
        raise NoticesError(
            f"{build_dir} is not a build directory of a Chromium tree, such as chromium-<tag>/src/out/mitto"
        )
    return src


def host_of(entries: Sequence[Entry]) -> str:
    """Returns the one Host that a part of the notices, as the host command prints it, is for.

    Raises:
        NoticesError: If the entries are not all for one Host.
    """
    hosts = {host for entry in entries for host in entry.hosts}
    if len(hosts) != 1 or any(len(entry.hosts) != 1 for entry in entries):
        raise NoticesError("a part of the notices must name one Host, and only that Host, in every entry")
    return hosts.pop()


def update(host: str, fresh: Sequence[Entry]) -> None:
    """Replaces one Host's part of THIRD_PARTY_NOTICES, and says what changed."""
    entries = parse(NOTICES_FILE.read_text(encoding="utf-8")) if NOTICES_FILE.is_file() else []
    changes = differences(entries, host, fresh)
    NOTICES_FILE.write_text(render(merge(entries, host, fresh)), encoding="utf-8", newline="\n")
    for line in changes:
        print(f"notices: {line}", file=sys.stderr)
    print(f"notices: {len(fresh)} entries linked on {HOST_NAMES[host]}, {len(changes)} changed", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    build_dir_help = "a build directory, such as chromium-<tag>/src/out/mitto"
    commands.add_parser("update", help=f"rewrite this Host's part of {NOTICES_FILE.name} from a build").add_argument(
        "build_dir", type=Path, help=build_dir_help
    )
    commands.add_parser("host", help="print only this Host's part, from a build").add_argument(
        "build_dir", type=Path, help=build_dir_help
    )
    commands.add_parser(
        "merge", help=f"rewrite a Host's part of {NOTICES_FILE.name} from what host printed"
    ).add_argument("part", type=Path, help="what the host command printed, on any Host")
    arguments = parser.parse_args()
    try:
        match arguments.command:
            case "update" | "host":
                build_dir: Path = arguments.build_dir
                fresh = host_entries(source_tree_of(build_dir), build_dir)
                if arguments.command == "host":
                    # As the file is written, whatever the console's encoding and line ends.
                    sys.stdout.buffer.write(render(fresh).encode())
                else:
                    update(export.current_host().name, fresh)
            case _:
                part: Path = arguments.part
                fresh = parse(part.read_text(encoding="utf-8"))
                update(host_of(fresh), fresh)
    except (NoticesError, export.ExportError) as error:
        print(f"notices: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
