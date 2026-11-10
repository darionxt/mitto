"""Updates the source export to a later build of the same milestone, from the diff between the two tags.

    MITTO_WORKSPACE=/absolute/path uv run python -m tools.workspace.update_export [--archive OLD_ARCHIVE]

A patch build changes few files of the tree, and seldom what the native core's build reads. The export of the build
before it then still names every file that the new build reads, on every Host, and only what those files say has
changed. So this takes each file of the old export, the pinned one unless --archive names another, from the tag of the
build in tools/workspace/chromium_tag: from chromium/src, or from the dependency that DEPS pins at that path, such as
V8 or BoringSSL. It applies tools/workspace/patches again to a patched file that upstream changed, writes again what
the hooks of DEPS write rather than git (the build's commit time, and V8's builtins profiles for the new V8), and pins
the toolchain of the new DEPS, as the merge does.

It stops before it writes anything, and new slices are needed, when the diff deletes a file of the export, changes one
of its build files (BUILD.gn, a .gn or .gni file, a .pydeps list), adds an include of a file that the export lacks or a
Python import, or when upstream changed a patched file so that its patch no longer applies. Those are the ways in which
a build can come to read a file that the export does not hold.

It needs git, the network, and MITTO_WORKSPACE for depot_tools, which resolves the toolchain's CIPD packages, and for
the shallow repositories that tools/workspace/compare keeps under compare/. It needs no checkout of either build. The
archive goes where the merge writes it, export/<build>/mitto-chromium-<build>.tar.xz. See docs/building.md.
"""

import argparse
import dataclasses
import difflib
import json
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import Any, cast

from tools.workspace import export

__all__ = [
    "GitDependency",
    "UpdateError",
    "added_imports",
    "build_file_changed",
    "git_dependencies",
    "included_names",
    "main",
    "missing_includes",
    "owner_of",
    "patched_paths",
    "update_export",
]

CHROMIUM_SRC_URL = "https://chromium.googlesource.com/chromium/src.git"

# What a hook of DEPS writes, rather than git. A file of the export that git does not hold must be one of these.
LASTCHANGE_COMMIT_TIME = "build/util/LASTCHANGE.committime"
PROFILES_DIRECTORY = "v8/tools/builtins-pgo/profiles"
PROFILES_URL = "https://storage.googleapis.com/chromium-v8-builtins-pgo/by-version/{version}/{name}"
V8_VERSION_FILE = "v8/include/v8-version.h"

# Build files, whose change can add a file to the build or a dependency to the graph.
BUILD_FILE_SUFFIXES = {".gn", ".gni", ".pydeps"}
# The root's .gn, whose name has no suffix in Python's eyes.
BUILD_FILE_NAMES = {".gn"}

# Files whose includes the preprocessor follows, or Torque's. The C++ library's own headers, such as
# third_party/libc++/src/include/vector, have no suffix.
INCLUDING_SUFFIXES = {
    "",
    ".c",
    ".cc",
    ".cpp",
    ".def",
    ".h",
    ".hpp",
    ".inc",
    ".inl",
    ".m",
    ".mm",
    ".S",
    ".s",
    ".asm",
    ".tq",
}
INCLUDE = re.compile(
    r"""^\s*#\s*(?:include|import|include_next)\s*[<"']([^>"']+)[>"']|__has_include(?:_next)?\s*\(\s*[<"]([^>"]+)[>"]"""
)

# Where a build puts what it generates, which an include can name without the export holding it.
GENERATED_PREFIXES = ("gen/", "torque-generated/")

PYTHON_IMPORT = re.compile(r"^\s*(?:import\s+\w|from\s+[\w.]+\s+import\s)")


class UpdateError(Exception):
    """Stops the script with a message for the person running it."""


@dataclasses.dataclass(frozen=True)
class GitDependency:
    """A git repository that DEPS pins at a path of the tree, at a revision."""

    url: str
    revision: str


@dataclasses.dataclass(frozen=True)
class Change:
    """What the diff between the two builds does to a file of the export."""

    path: str
    new: bytes


def git_dependencies(deps: Mapping[str, Any]) -> dict[str, GitDependency]:
    """Returns the git repositories that DEPS pins, by their path relative to src."""
    found: dict[str, GitDependency] = {}
    entries: Mapping[str, Any] = deps["deps"]
    for path, entry in entries.items():
        value: object = cast("dict[str, object]", entry).get("url") if isinstance(entry, dict) else entry
        if not isinstance(value, str) or "@" not in value or not path.startswith("src/"):
            continue
        url, _, revision = value.rpartition("@")
        found[path.removeprefix("src/")] = GitDependency(url, revision)
    return found


def owner_of(path: str, dependency_paths: Iterable[str]) -> str:
    """Returns the path of the dependency that holds a file, or "" for chromium/src itself."""
    owners = [dependency for dependency in dependency_paths if path.startswith(dependency + "/")]
    return max(owners, key=len, default="")


def build_file_changed(path: str) -> bool:
    name = PurePosixPath(path).name
    return name in BUILD_FILE_NAMES or PurePosixPath(path).suffix in BUILD_FILE_SUFFIXES


def included_names(text: str) -> set[str]:
    """Returns what the include directives and __has_include checks of a source file name."""
    names: set[str] = set()
    for line in text.splitlines():
        for match in INCLUDE.finditer(line):
            names.add(match[1] or match[2])
    return names


def missing_includes(path: str, old: bytes, new: bytes, exported: Iterable[str]) -> set[str]:
    """Returns what the new version of a file includes and the old did not, which no file of the export can be.

    An include names a file relative to one of many directories, so it counts as found when a file of the export
    ends with it. A generated file is never in the export.
    """
    if PurePosixPath(path).suffix not in INCLUDING_SUFFIXES:
        return set()
    added = included_names(new.decode(errors="replace")) - included_names(old.decode(errors="replace"))
    held = list(exported)
    return {
        name
        for name in added
        if not name.startswith(GENERATED_PREFIXES)
        and not any(file == name or file.endswith("/" + name) for file in held)
    }


def added_imports(path: str, old: bytes, new: bytes) -> list[str]:
    """Returns the import lines that the new version of a Python file adds."""
    if PurePosixPath(path).suffix != ".py":
        return []
    diff = difflib.ndiff(old.decode(errors="replace").splitlines(), new.decode(errors="replace").splitlines())
    return [line[2:].strip() for line in diff if line.startswith("+ ") and PYTHON_IMPORT.match(line[2:])]


def patched_paths(patch: str) -> set[str]:
    """Returns the files that a patch changes."""
    return set(re.findall(r"^diff --git a/(\S+) b/", patch, re.MULTILINE))


# Git.


def git(repository: Path, *arguments: str) -> str:
    return export.output_of(["git", "-C", repository, *arguments])


def git_bytes(repository: Path, *arguments: str) -> bytes:
    return subprocess.run(["git", "-C", repository, *arguments], check=True, capture_output=True).stdout


def has_commit(repository: Path, revision: str) -> bool:
    found = subprocess.run(
        ["git", "-C", repository, "rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}"], capture_output=True
    )
    return found.returncode == 0


def repository_at(directory: Path, url: str) -> Path:
    """Returns a repository that is only fetched into, as tools/workspace/compare keeps them."""
    if not (directory / ".git").exists():
        directory.mkdir(parents=True, exist_ok=True)
        export.check_call(["git", "init", "--quiet", directory])
        export.check_call(["git", "-C", directory, "config", "remote.origin.url", url])
    return directory


def fetch_tag(repository: Path, tag: str) -> str:
    if not has_commit(repository, f"refs/tags/{tag}"):
        export.note(f"fetching chromium/src {tag}; a first tag of a milestone takes minutes")
        export.check_call(
            [
                "git",
                "-C",
                repository,
                "fetch",
                "--quiet",
                "--depth=1",
                "--no-tags",
                "origin",
                f"refs/tags/{tag}:refs/tags/{tag}",
            ]
        )
    return f"refs/tags/{tag}"


def fetch_revision(repository: Path, revision: str) -> str:
    if not has_commit(repository, revision):
        export.check_call(["git", "-C", repository, "fetch", "--quiet", "--depth=1", "--no-tags", "origin", revision])
    return revision


def changed_files(repository: Path, old: str, new: str) -> dict[str, str]:
    """Returns each file that differs between two commits, with A, M or D, and no renames."""
    listing = git(repository, "diff", "--name-status", "--no-renames", "-z", old, new).split("\0")
    return dict(zip(listing[1::2], listing[0::2], strict=False))


def tracked_files(repository: Path, revision: str) -> set[str]:
    return set(git(repository, "ls-tree", "-r", "--name-only", "-z", revision).split("\0")) - {""}


# The update.


def v8_version(header: str) -> str:
    parts = [
        re.search(rf"^#define V8_{name} (\d+)", header, re.MULTILINE)
        for name in ("MAJOR_VERSION", "MINOR_VERSION", "BUILD_NUMBER", "PATCH_LEVEL")
    ]
    if not all(parts):
        raise UpdateError(f"{V8_VERSION_FILE} does not say which V8 it is")
    return ".".join(part[1] for part in parts if part)


def download_bytes(url: str) -> bytes:
    with tempfile.TemporaryDirectory() as temporary:
        file = Path(temporary) / "download"
        export.note(f"downloading {url}")
        export.check_call([export.system_program("curl"), *export.CURL_ARGUMENTS, "--output", file, url])
        return file.read_bytes()


def hook_output(path: str, src: Path, commit_time: int) -> bytes | None:
    """Returns what a hook of DEPS writes into a file of the export for the new build, or None for no change.

    Only a file of chromium/src or of a dependency that moved comes here.
    """
    if path == LASTCHANGE_COMMIT_TIME:
        return str(commit_time).encode()
    if path in export.PER_HOST_FILES:
        # gclient writes it from DEPS's gclient_gn_args, whose checkout_* values decide nothing in this graph.
        return None
    if path.startswith(PROFILES_DIRECTORY + "/"):
        version = v8_version((src / V8_VERSION_FILE).read_text())
        return download_bytes(PROFILES_URL.format(version=version, name=PurePosixPath(path).name))
    raise UpdateError(f"{path} is in the export but not in git; say in this script which hook of DEPS writes it")


def exported_paths(src: Path) -> list[str]:
    return sorted(export.merge_slices([src.parent]))


def diff_changes(
    workspace: Path, src: Path, old_tag: str, new_tag: str, exported: Sequence[str]
) -> tuple[list[Change], set[str], list[str], int, Mapping[str, Any]]:
    """Returns the new content of each exported file that git changed, the untracked ones, what calls for slices,
    the new build's commit time and its DEPS.

    A dependency that did not move changed none of its files, nor what a hook writes into it, such as V8's profiles,
    so only chromium/src and the dependencies that moved are fetched.
    """
    compare = workspace / "compare"
    src_repository = repository_at(compare / "src", CHROMIUM_SRC_URL)
    old_ref, new_ref = fetch_tag(src_repository, old_tag), fetch_tag(src_repository, new_tag)
    old_deps = export.evaluate_deps(git(src_repository, "show", f"{old_ref}:DEPS"))
    new_deps = export.evaluate_deps(git(src_repository, "show", f"{new_ref}:DEPS"))
    old_dependencies, new_dependencies = git_dependencies(old_deps), git_dependencies(new_deps)
    commit_time = int(git(src_repository, "log", "-1", "--format=%ct", new_ref).strip())

    by_owner: dict[str, list[str]] = {}
    for path in exported:
        by_owner.setdefault(owner_of(path, old_dependencies), []).append(path)

    changes: list[Change] = []
    untracked: set[str] = set()
    reasons: list[str] = []
    for owner, paths in sorted(by_owner.items()):
        prefix = owner + "/" if owner else ""
        if owner:
            before, after = old_dependencies[owner], new_dependencies.get(owner)
            if after is None or after.url != before.url:
                reasons.append(f"DEPS no longer pins {before.url} at {owner}")
                continue
            if after.revision == before.revision:
                continue
            export.note(f"{owner} moved from {before.revision[:12]} to {after.revision[:12]}")
            repository = repository_at(compare / "deps" / owner.replace("/", "_"), before.url)
            old_rev, new_rev = fetch_revision(repository, before.revision), fetch_revision(repository, after.revision)
        else:
            repository, old_rev, new_rev = src_repository, old_ref, new_ref
        tracked = tracked_files(repository, old_rev)
        statuses = changed_files(repository, old_rev, new_rev)
        for path in paths:
            relative = path.removeprefix(prefix)
            status = statuses.get(relative)
            if relative not in tracked:
                untracked.add(path)
            elif status == "D":
                reasons.append(f"{path} is gone in {new_tag}")
            elif status == "M" and (src / path).is_symlink():
                reasons.append(f"{path}, a symbolic link, changed")
            elif status == "M":
                changes.append(Change(path, git_bytes(repository, "show", f"{new_rev}:{relative}")))
            elif status is not None:
                # Such as T, a file that became a link.
                reasons.append(f"{path} changed its type")
    return changes, untracked, reasons, commit_time, new_deps


def apply_patches(src: Path, changes: Sequence[Change], patches: Sequence[Path]) -> None:
    """Applies a patch again to each file that it changes and that upstream changed too."""
    changed = {change.path for change in changes}
    for patch in patches:
        touched = patched_paths(patch.read_text())
        if not touched & changed:
            continue
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            for path in touched:
                export.copy_files(src, [path], tree)
            applied = subprocess.run(["git", "apply", "--quiet", patch.resolve()], cwd=tree, capture_output=True)
            if applied.returncode != 0:
                raise UpdateError(f"{patch.name} no longer applies to the new build; update it as in a run")
            for path in touched:
                (src / path).write_bytes((tree / path).read_bytes())


def update_export(workspace: Path, archive: Path, new_tag: str) -> Path:
    patches = sorted(export.PATCHES.glob("*.patch"))
    with tempfile.TemporaryDirectory(dir=workspace, prefix=".update-") as temporary:
        export.check_call([export.system_program("tar"), "-xf", archive, "-C", temporary])
        roots = list(Path(temporary).glob("chromium-*/export.json"))
        if len(roots) != 1:
            raise UpdateError(f"{archive} is not a source export")
        old_root = roots[0].parent
        metadata = json.loads(roots[0].read_text())
        old_tag = metadata["chromium_tag"]
        if old_tag.split(".")[:3] != new_tag.split(".")[:3]:
            raise UpdateError(f"{old_tag} and {new_tag} are not builds of one milestone's branch; make new slices")
        if metadata["patches"] != {patch.name: export.sha256_of(patch) for patch in patches}:
            raise UpdateError("the export was made with other patches; make new slices")
        src = old_root / "src"
        exported = exported_paths(src)

        # The old export's patched files are the patched versions; git holds the unpatched ones.
        changes, untracked, reasons, commit_time, new_deps = diff_changes(workspace, src, old_tag, new_tag, exported)
        for change in changes:
            old = (src / change.path).read_bytes()
            new = change.new
            if build_file_changed(change.path):
                reasons.append(f"{change.path}, a build file, changed")
            reasons.extend(
                f"{change.path} now includes {name}, which the export lacks"
                for name in sorted(missing_includes(change.path, old, new, exported))
            )
            reasons.extend(f"{change.path} adds `{line}`" for line in added_imports(change.path, old, new))
        if reasons:
            raise UpdateError("make new slices, because:\n  " + "\n  ".join(reasons))

        for change in changes:
            (src / change.path).write_bytes(change.new)
        apply_patches(src, changes, patches)
        for path in sorted(untracked):
            content = hook_output(path, src, commit_time)
            if content is not None:
                (src / path).write_bytes(content)

        files = export.merge_slices([old_root])
        stores = export.PackageStores(export.resolve_with_depot_tools(workspace), export.sha256_of_download)
        new_metadata = {
            "chromium_tag": new_tag,
            "patches": metadata["patches"],
            "updated_from": {"chromium_tag": old_tag, "sha256": export.sha256_of(archive)},
            "traced_outputs": {},
            # The Hosts of the old export, whose slices named its files.
            "toolchain": {
                host.name: export.toolchain_pins(new_deps, files, host, stores)
                for host in export.HOSTS
                if host.name in metadata["toolchain"]
            },
        }
        extras = {
            "README.md": export.ARCHIVE_README.format(tag=new_tag).encode(),
            "export.json": (json.dumps(new_metadata, indent=2) + "\n").encode(),
        }
        extras |= {f"patches/{patch.name}": patch.read_bytes() for patch in patches}
        destination = export.export_dir(workspace, new_tag) / export.archive_name(new_tag)
        destination.parent.mkdir(parents=True, exist_ok=True)
        export.note(f"{len(changes)} files changed; compressing {len(files)} files")
        export.write_archive(destination, f"chromium-{new_tag}", files, extras, commit_time)
    size = destination.stat().st_size
    print(f"{destination}\n  {export.human_size(size)}, {len(files)} files, SHA-256 {export.sha256_of(destination)}")
    for change in changes:
        print(f"  changed {change.path}")
    return destination


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=Path, help="the export to update, instead of the pinned one")
    arguments = parser.parse_args(argv)
    try:
        workspace = export.workspace_dir()
        archive = arguments.archive.resolve() if arguments.archive else export.archive_from_pin(workspace / "export")
        update_export(workspace, archive, export.read_tag())
    except (UpdateError, export.ExportError) as error:
        print(f"update_export: error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"update_export: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
