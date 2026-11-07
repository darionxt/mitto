#!/usr/bin/env python3
"""Makes the trimmed source export of the native core, and builds from it.

A hosted runner cannot check out all of Chromium, so CI builds the native core
from an archive that holds only what that build reads. Each Host reads different
files, and gn on one Host knows nothing of another's, so the list comes from a
real build on each Host:

    export.py slice           on each Host, in a workspace set up by bootstrap
    export.py merge           on one Host, once it holds every Host's slice
    export.py build <dir>     anywhere, with no Chromium checkout

The toolchain stays out of the archive. It is what Chromium's own bots use, so
the archive names it, by hash, in Chromium's own storage. See docs/building.md.

This file needs only the standard library, so that a runner can use it before
anything else is installed.
"""

import argparse
import ast
import base64
import dataclasses
import filecmp
import hashlib
import io
import json
import lzma
import os
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from collections import Counter
from collections.abc import Callable, Iterable, Iterator, Mapping
from pathlib import Path, PurePosixPath
from typing import Any, NoReturn, cast

REPO = Path(__file__).resolve().parents[2]
TAG_FILE = REPO / "tools/workspace/chromium_tag"
ARGS_GN = REPO / "tools/workspace/args.gn"
PATCHES = REPO / "tools/workspace/patches"
NATIVE = REPO / "native"
PIN_FILE = REPO / "tools/workspace/source_export.json"

# The time that the files of a CIPD package get, in seconds since the epoch.
CIPD_FILE_TIME = 1_000_000_000

# GitHub refuses a bigger release asset.
ASSET_LIMIT = 2 * 1024**3

GN_ROOT = ["--root-target=//mitto", "--root-pattern=//mitto:*"]

# A file that gclient writes per Host. It only sets checkout_* variables, and
# those decide nothing in the native core's build graph.
PER_HOST_FILES = {"build/config/gclient_args.gni"}

# What no list of a build's inputs names. The merge reads the toolchain pins from
# DEPS, and gn only checks that the other file exists, with path_exists().
ALWAYS_KEPT = {"DEPS", "build/modules/linux/BUILD.gn"}

# Build output, and what comes from this repository rather than the export.
NEVER_KEPT_DIRS = ("out", "mitto")

# A directory of the checkout that the toolchain fills. A slice leaves these out
# and the build fetches them again.
TOOLCHAIN_DIRS = (
    "buildtools/linux64",
    "buildtools/mac",
    "buildtools/third_party/mold/cipd",
    "third_party/cpython3",
    "third_party/llvm-build",
    "third_party/ninja",
    "third_party/rust-toolchain",
)
SYSROOT_DIR = re.compile(r"build/linux/[^/]+-sysroot(/|$)")

LICENCE_NAME = re.compile(
    r"^((mit|bsd|apache)-)?(licen[cs]es?|copying|notice|copyright|patents?|authors|contributors|unlicense)([._-].*)?$",
    re.IGNORECASE,
)
README_CHROMIUM = "README.chromium"
RUST_CRATES = "third_party/rust/chromium_crates_io/vendor"

# Chromium's own build files for a project that lies under third_party/. They
# come under Chromium's licence at the top of the tree.
THIRD_PARTY_WITHOUT_LICENCE = {"buildtools/third_party/libc++", "buildtools/third_party/libc++abi"}

# The first bytes of an ELF file, a Mach-O file and a universal Mach-O file.
EXECUTABLE_MAGIC = (b"\x7fELF", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


@dataclasses.dataclass(frozen=True)
class GcsPackage:
    """A package that DEPS pins in Google Storage, by a pattern for its object."""

    path: str
    object_pattern: str


@dataclasses.dataclass(frozen=True)
class CipdPackage:
    """A package that DEPS pins in CIPD, Chromium's package store."""

    path: str


@dataclasses.dataclass(frozen=True)
class BuildConfig:
    """A way to build the native core: GN args on top of args.gn, in a build directory of its own."""

    name: str
    args: str = ""


# The build that ships.
PLAIN = BuildConfig("mitto")
ASAN = BuildConfig("asan", "is_asan = true\nis_lsan = true\n")
UBSAN = BuildConfig("ubsan", "is_ubsan = true\nis_ubsan_no_recover = true\n")
# The fuzz target has a build of its own, because Chromium's fuzzing builds define
# FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION, which changes BoringSSL's random numbers
# among other things. Its owners files would ask git, and an export has no git.
FUZZ = BuildConfig("fuzz", "is_asan = true\nuse_libfuzzer = true\ngenerate_fuzzer_owners = false\n")
CONFIGS = (PLAIN, ASAN, UBSAN, FUZZ)


@dataclasses.dataclass(frozen=True)
class Host:
    name: str
    gn: str
    cipd_platform: str
    packages: tuple[GcsPackage | CipdPackage, ...]
    library: str
    configs: tuple[BuildConfig, ...]

    @property
    def cipd_arch(self) -> str:
        return self.cipd_platform.rpartition("-")[2]


LINUX = Host(
    name="linux",
    gn="buildtools/linux64/gn",
    cipd_platform="linux-amd64",
    packages=(
        GcsPackage("third_party/llvm-build/Release+Asserts", r"Linux_x64/clang-llvmorg-.*"),
        GcsPackage("third_party/rust-toolchain", r"Linux_x64/rust-toolchain-.*"),
        GcsPackage("build/linux/debian_bullseye_amd64-sysroot", r".*"),
        CipdPackage("buildtools/linux64"),
        CipdPackage("buildtools/third_party/mold/cipd"),
        CipdPackage("third_party/ninja"),
        CipdPackage("third_party/cpython3/host"),
    ),
    library="libmitto_core.so",
    configs=CONFIGS,
)
MAC = Host(
    name="mac",
    gn="buildtools/mac/gn",
    cipd_platform="mac-arm64",
    packages=(
        GcsPackage("third_party/llvm-build/Release+Asserts", r"Mac_arm64/clang-llvmorg-.*"),
        GcsPackage("third_party/llvm-build/Release+Asserts", r"Mac_arm64/llvmobjdump-llvmorg-.*"),
        GcsPackage("third_party/rust-toolchain", r"Mac_arm64/rust-toolchain-.*"),
        CipdPackage("buildtools/mac"),
        CipdPackage("third_party/ninja"),
        CipdPackage("third_party/cpython3/host"),
    ),
    library="libmitto_core.dylib",
    configs=(PLAIN,),
)
HOSTS = (LINUX, MAC)

MACH_O_MAGIC = b"\xcf\xfa\xed\xfe"
MACH_O_HEADER_SIZE = 32
MACH_O_SEGMENT_COMMAND = 0x19
MACH_O_SEGMENT_HEADER_SIZE = 72
MACH_O_SECTION_HEADER_SIZE = 80
# The section types that take no room in the file.
MACH_O_ZERO_FILL_TYPES = {0x1, 0xC, 0x12}

ARCHIVE_README = """\
This is the part of Chromium {tag} that mitto's native core needs in order to
build, on Linux x86_64 and on macOS arm64. Everything else in Chromium is left
out. See docs/building.md in https://github.com/darionxt/mitto, whose script
tools/workspace/export.py made this archive.

src/         Chromium's files, with the patches below applied and no other change
patches/     every change to those files, each with its reason
export.json  where to get the toolchain, which this archive does not include

The licences are Chromium's own: src/LICENSE, and the licence files in the
directories of the third-party projects under src/.
"""

# Run by the Python of every build step during a traced build. Only Python's
# audit events see what a script imports and reads on both Hosts without root.
TRACE_HOOK = """\
import os
import sys

_log = os.open(
    os.path.join(os.environ["MITTO_EXPORT_TRACE"], "%d.log" % os.getpid()),
    os.O_WRONLY | os.O_CREAT | os.O_APPEND,
    0o644,
)


def _record(path):
    path = os.path.abspath(os.fsdecode(path))
    os.write(_log, path.encode("utf-8", "surrogateescape") + b"\\n")


def _hook(event, args):
    if event == "open" and isinstance(args[0], (str, bytes)):
        _record(args[0])


# A script can use a file without opening it, such as to read its modification
# time, and then only its arguments name that file.
for _argument in sys.argv[1:]:
    _path = _argument.partition("=")[2] or _argument
    if os.path.isfile(_path):
        _record(_path)

sys.addaudithook(_hook)
"""


class ExportError(Exception):
    """Stops the script with a message for the person running it."""


def die(message: str) -> NoReturn:
    raise ExportError(message)


def note(message: str) -> None:
    print(f"export: {message}", file=sys.stderr, flush=True)


def current_host() -> Host:
    match sys.platform, platform.machine():
        case "linux", "x86_64":
            return LINUX
        case "darwin", "arm64":
            return MAC
        case system, machine:
            die(f"unsupported host {system}/{machine}; use macOS arm64 or Linux x86_64")


def read_tag() -> str:
    tag = TAG_FILE.read_text().strip()
    if not re.fullmatch(r"\d+\.0\.\d+\.\d+", tag):
        die(f"{TAG_FILE} holds '{tag}', not a Chromium version")
    return tag


def workspace_dir() -> Path:
    value = os.environ.get("MITTO_WORKSPACE")
    if not value:
        die("set MITTO_WORKSPACE to the workspace that tools/workspace/bootstrap set up")
    return Path(value).resolve()


def export_dir(workspace: Path, tag: str) -> Path:
    return workspace / "export" / tag


def archive_name(tag: str) -> str:
    return f"mitto-chromium-{tag}.tar.xz"


def human_size(size: float) -> str:
    for unit in ("B", "KiB", "MiB"):
        if size < 1024:
            return f"{size:.0f} {unit}"
        size /= 1024
    return f"{size:.2f} GiB"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def library_fingerprint(library: Path) -> str:
    """Returns a hash of a library that does not depend on where it was built.

    A Mach-O file names each of its object files by absolute path for the
    debugger, which changes its symbol table, its identity and its signature. So
    that format gets a hash of its sections alone. An ELF file has no such paths.
    """
    data = library.read_bytes()
    if data[:4] != MACH_O_MAGIC:
        return hashlib.sha256(data).hexdigest()
    digest = hashlib.sha256()
    (command_count,) = struct.unpack_from("<I", data, 16)
    command = MACH_O_HEADER_SIZE
    for _ in range(command_count):
        kind, command_size = struct.unpack_from("<2I", data, command)
        if kind == MACH_O_SEGMENT_COMMAND:
            (section_count,) = struct.unpack_from("<I", data, command + 64)
            for index in range(section_count):
                header = command + MACH_O_SEGMENT_HEADER_SIZE + index * MACH_O_SECTION_HEADER_SIZE
                names_and_address, size, offset = struct.unpack_from("<40sQI", data, header)
                (flags,) = struct.unpack_from("<I", data, header + 64)
                digest.update(names_and_address)
                digest.update(size.to_bytes(8, "little"))
                if flags & 0xFF not in MACH_O_ZERO_FILL_TYPES:
                    digest.update(data[offset : offset + size])
        command += command_size
    return digest.hexdigest()


def check_call(command: list[str | Path], cwd: Path | None = None, env: Mapping[str, str] | None = None) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True)


def output_of(command: list[str | Path], cwd: Path | None = None, env: Mapping[str, str] | None = None) -> str:
    return subprocess.run(
        command, cwd=cwd, env=env, check=True, capture_output=True, encoding="utf-8", errors="surrogateescape"
    ).stdout


def build_environment() -> dict[str, str]:
    """Returns the environment of a build, which depot_tools must not be part of.

    A runner has no depot_tools, so a traced build that used it would hide a
    dependency that only the runner would then find.
    """
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(
        entry for entry in env.get("PATH", "").split(os.pathsep) if "depot_tools" not in Path(entry).parts
    )
    for name in ("PYTHONHOME", "PYTHONPATH", "VIRTUAL_ENV"):
        env.pop(name, None)
    return env


# Reading what a build used.


def paths_in_project(project: Mapping[str, Any]) -> set[str]:
    """Returns every source-absolute file that gn read or that a target declares.

    Declared headers are kept even when nothing includes them yet, so that native
    code can include more of a dependency it already has without a new export.
    """
    declared: list[str] = list(project["build_settings"]["gen_input_files"])
    for target in project["targets"].values():
        for key in ("sources", "public", "inputs"):
            # `public` is the string "*" when a target does not restrict it.
            value = target.get(key, [])
            if isinstance(value, list):
                declared.extend(cast("list[str]", value))
        if "script" in target:
            declared.append(target["script"])
    return {path.removeprefix("//") for path in declared if path.startswith("//")}


def paths_in_ninja_deps(text: str) -> set[str]:
    """Returns the paths in the output of `ninja -t deps`, relative to the build directory."""
    return {line.strip() for line in text.splitlines() if line.startswith((" ", "\t")) and line.strip()}


def paths_in_depfile(text: str) -> set[str]:
    """Returns the prerequisites in a Makefile-style depfile."""
    paths: set[str] = set()
    for rule in text.replace("\\\n", " ").splitlines():
        _, separator, prerequisites = rule.partition(": ")
        if not separator:
            continue
        for path in re.split(r"(?<!\\)\s+", prerequisites.strip()):
            if path:
                paths.add(path.replace("\\ ", " ").replace("$$", "$"))
    return paths


def paths_in_commands(text: str) -> set[str]:
    """Returns what the commands of a build name in the source tree.

    Compilers and linkers read files that no depfile lists, such as warning
    suppression lists and symbol lists, and they name them only in a flag.
    """
    return set(re.findall(r"\.\./\.\./[^\s\"'`;:,=]+", text))


def system_include_dirs(text: str) -> set[str]:
    """Returns the directories that the commands of a build search as system headers.

    Compilers run with -MMD, which leaves headers found this way out of the depfile.
    """
    return set(re.findall(r"-isystem\s*(\.\./\.\./[^\s\"']+)", text))


def source_relative(path: str, src: Path, base: Path) -> str | None:
    """Returns a path relative to the source root, or None if it lies outside."""
    absolute = os.path.normpath(base / path)
    root = str(src) + os.sep
    return absolute.removeprefix(root) if absolute.startswith(root) else None


def is_kept_path(path: str) -> bool:
    """Says whether a file of the source tree can go into the export at all."""
    parts = PurePosixPath(path).parts
    if parts[0] in NEVER_KEPT_DIRS or "__pycache__" in parts or ".git" in parts:
        return False
    if any(path == directory or path.startswith(directory + "/") for directory in TOOLCHAIN_DIRS):
        return False
    return not SYSROOT_DIR.match(path)


def with_symlinks(src: Path, path: str) -> set[str]:
    """Returns a path and, if it goes through symlinks, the links and their targets."""
    parts = PurePosixPath(path).parts
    for index in range(1, len(parts) + 1):
        current = "/".join(parts[:index])
        link = src / current
        if link.is_symlink():
            target = os.path.normpath(link.parent / link.readlink())
            if not target.startswith(str(src) + os.sep):
                die(f"{current} is a symlink that leaves the source tree")
            rest = parts[index:]
            resolved = os.path.relpath(target, src)
            return {current} | with_symlinks(src, "/".join((resolved, *rest)))
    return {path}


def licence_files(src: Path, kept: Iterable[str]) -> set[str]:
    """Returns the licence files that apply to the kept files.

    A licence applies to the directory it is in and everything below, so each
    directory on the way to a kept file gives up its own. README.chromium names a
    project's licence when the file has an unusual name or lies elsewhere.
    """
    directories = {str(parent) for path in kept for parent in PurePosixPath(path).parents}
    licences: set[str] = set()
    for directory in directories:
        for entry in (src / directory).iterdir():
            if entry.is_dir():
                continue
            name = str(PurePosixPath(directory) / entry.name)
            if LICENCE_NAME.match(entry.name):
                licences.add(name)
            elif entry.name == README_CHROMIUM:
                licences.add(name)
                licences |= _named_licence_files(src, directory, entry.read_text(errors="replace"))
    return {path for path in licences if is_kept_path(path)}


def _files_under(directory: Path) -> Iterator[Path]:
    for root, _, names in os.walk(directory):
        for name in names:
            yield Path(root) / name


def _named_licence_files(src: Path, directory: str, readme: str) -> set[str]:
    named: set[str] = set()
    for line in readme.splitlines():
        field, separator, value = line.partition(":")
        if not separator or field.strip().lower() != "license file":
            continue
        for item in value.split(","):
            item = item.strip()
            # A leading slash means the top of the source tree, as in GN.
            path = item.lstrip("/") if item.startswith("/") else str(PurePosixPath(directory) / item)
            path = os.path.normpath(path)
            if item and not path.startswith("..") and (src / path).is_file():
                named.add(path)
    return named


def _third_party_project(path: str) -> str | None:
    """Returns the project that a file belongs to, if it belongs to one."""
    parts = PurePosixPath(path).parts
    if path.startswith(RUST_CRATES + "/") and len(parts) > RUST_CRATES.count("/") + 2:
        return "/".join(parts[: RUST_CRATES.count("/") + 2])
    for index in range(len(parts) - 3, -1, -1):
        if parts[index] == "third_party":
            return "/".join(parts[: index + 2])
    return None


def third_party_without_licence(files: Iterable[str]) -> list[str]:
    """Returns the third-party projects in the export that no licence file covers.

    A licence file in a project, or in a project that it is part of, covers it.
    The one at the top of the tree is Chromium's and does not count.
    """
    kept = list(files)
    projects = {project for path in kept if (project := _third_party_project(path))}
    covering = {str(PurePosixPath(path).parent) for path in kept if LICENCE_NAME.match(PurePosixPath(path).name)}
    covering.discard(".")
    missing = {
        project
        for project in projects
        if not any(
            directory == project or directory.startswith(project + "/") or project.startswith(directory + "/")
            for directory in covering
        )
    }
    return sorted(missing - THIRD_PARTY_WITHOUT_LICENCE)


# Slicing one Host's build out of its checkout.


def checkout_of(workspace: Path, tag: str) -> Path:
    src = workspace / f"chromium-{tag}" / "src"
    if not (src / "DEPS").is_file():
        die(f"{src} is not a checkout; run tools/workspace/bootstrap first")
    return src


def same_tree(first: Path, second: Path) -> bool:
    """Says whether two directories hold the same files with the same contents."""
    listing = {str(path.relative_to(first)) for path in _files_under(first)}
    if listing != {str(path.relative_to(second)) for path in _files_under(second)}:
        return False
    _, mismatch, errors = filecmp.cmpfiles(first, second, listing, shallow=False)
    return not mismatch and not errors


def check_checkout(src: Path) -> None:
    """Stops unless the checkout is the patched tree with this repository's native code."""
    for patch in sorted(PATCHES.glob("*.patch")):
        applied = subprocess.run(["git", "-C", src, "apply", "--reverse", "--check", patch], capture_output=True)
        if applied.returncode != 0:
            die(f"{patch.name} is not applied to {src}; rerun tools/workspace/bootstrap")
    if not (src / "mitto").is_dir() or not same_tree((src / "mitto").resolve(), NATIVE):
        die(f"{src / 'mitto'} differs from {NATIVE}; rerun tools/workspace/bootstrap from this repository")


def write_args(config: BuildConfig, build_dir: Path) -> None:
    (build_dir / "args.gn").write_text(ARGS_GN.read_text() + config.args)


def traced_build(src: Path, host: Host, config: BuildConfig, build_dir: Path, trace_dir: Path) -> None:
    """Builds the native core from nothing, so that every file it needs gets read."""
    site = trace_dir / "site"
    logs = trace_dir / "logs"
    site.mkdir(exist_ok=True)
    (site / "sitecustomize.py").write_text(TRACE_HOOK)
    logs.mkdir(exist_ok=True)
    env = build_environment()
    env |= {
        "MITTO_EXPORT_TRACE": str(logs),
        "PYTHONPATH": str(site),
        # Python reads a module's source unless it has compiled it before, and
        # only the read of the source shows which file the module came from.
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONPYCACHEPREFIX": str(trace_dir / "pycache"),
    }
    shutil.rmtree(build_dir, ignore_errors=True)
    build_dir.mkdir(parents=True)
    write_args(config, build_dir)
    relative = build_dir.relative_to(src)
    note(f"generating the traced {config.name} build")
    check_call([src / host.gn, "gen", relative, *GN_ROOT, "--ide=json"], cwd=src, env=env)
    note("building; a first build takes minutes")
    check_call([src / "third_party/ninja/ninja", "-C", relative, "mitto"], cwd=src, env=env)


def paths_read_by_python(logs: Path, src: Path) -> set[str]:
    paths: set[str] = set()
    for log in logs.glob("*.log"):
        for line in log.read_text(errors="surrogateescape").splitlines():
            relative = source_relative(line, src, src)
            if relative is not None:
                paths.add(relative)
    return paths


def paths_used_by_build(src: Path, build_dir: Path, logs: Path) -> set[str]:
    """Returns every file of the source tree that the finished build used."""
    ninja = src / "third_party/ninja/ninja"
    paths = paths_in_project(json.loads((build_dir / "project.json").read_text()))
    commands = output_of([ninja, "-C", build_dir, "-t", "commands", "mitto"])
    relative_to_build = paths_in_ninja_deps(output_of([ninja, "-C", build_dir, "-t", "deps"]))
    relative_to_build |= paths_in_commands(commands)
    for directory in system_include_dirs(commands):
        for file in _files_under(build_dir / directory):
            relative_to_build.add(str(file))
    for depfile in build_dir.rglob("*.d"):
        if depfile.is_file():
            relative_to_build |= paths_in_depfile(depfile.read_text(errors="replace"))
    for path in relative_to_build:
        relative = source_relative(path, src, build_dir)
        if relative is not None:
            paths.add(relative)
    return paths | paths_read_by_python(logs, src)


def files_to_keep(src: Path, used: Iterable[str]) -> set[str]:
    """Turns what a build used into what the export must hold."""
    paths = {path for path in ALWAYS_KEPT if (src / path).is_file()}
    for path in used:
        if not is_kept_path(path):
            continue
        for resolved in with_symlinks(src, path):
            target = src / resolved
            if is_kept_path(resolved) and (target.is_symlink() or target.is_file()):
                paths.add(resolved)
    paths |= licence_files(src, paths)
    for path in paths:
        target = src / path
        if not target.is_symlink():
            with target.open("rb") as file:
                if file.read(4) in EXECUTABLE_MAGIC:
                    die(f"{path} is a prebuilt program; add its package to the toolchain in this script")
    return paths


def copy_files(src: Path, paths: Iterable[str], destination: Path) -> None:
    for path in paths:
        source = src / path
        target = destination / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            target.symlink_to(source.readlink())
        else:
            shutil.copyfile(source, target)
            target.chmod(0o755 if source.stat().st_mode & stat.S_IXUSR else 0o644)


def make_slice(workspace: Path, tag: str, host: Host) -> Path:
    src = checkout_of(workspace, tag)
    check_checkout(src)
    destination = export_dir(workspace, tag) / host.name
    used: set[str] = set()
    libraries: dict[str, str] = {}
    with tempfile.TemporaryDirectory(dir=workspace) as temporary:
        trace_dir = Path(temporary)
        for config in host.configs:
            build_dir = src / "out" / f"export-trace-{config.name}"
            traced_build(src, host, config, build_dir, trace_dir)
            used |= paths_used_by_build(src, build_dir, trace_dir / "logs")
            libraries[config.name] = library_fingerprint(build_dir / host.library)
            note(f"{config.name} {host.library}: fingerprint {libraries[config.name]}")
            shutil.rmtree(build_dir)
    paths = files_to_keep(src, used)
    shutil.rmtree(destination, ignore_errors=True)
    copy_files(src, paths, destination / "src")
    size = sum((destination / "src" / path).lstat().st_size for path in paths)
    (destination / "slice.json").write_text(
        json.dumps(
            {
                "chromium_tag": tag,
                "host": host.name,
                "commit_time": int(output_of(["git", "-C", src, "log", "-1", "--format=%ct"]).strip()),
                "patches": {patch.name: sha256_of(patch) for patch in sorted(PATCHES.glob("*.patch"))},
                "libraries": libraries,
            },
            indent=2,
        )
        + "\n"
    )
    note(f"{destination}: {len(paths)} files, {human_size(size)}")
    return destination


# Merging the slices of all Hosts into the export.


def evaluate_deps(text: str) -> dict[str, Any]:
    """Returns the `vars` and `deps` of a DEPS file, without running the file."""
    scope: dict[str, Any] = {}

    def value(node: ast.expr) -> Any:
        match node:
            case ast.Constant(value=constant):
                return constant
            case ast.List(elts=items):
                return [value(item) for item in items]
            case ast.Dict(keys=keys, values=values):
                return {value(key): value(item) for key, item in zip(keys, values, strict=True) if key is not None}
            case ast.BinOp(left=left, op=ast.Add(), right=right):
                return value(left) + value(right)
            case ast.Call(func=ast.Name(id="Var"), args=[name]):
                return scope["vars"][value(name)]
            case ast.Call(func=ast.Name(id="Str"), args=[string]):
                return value(string)
            case _:
                die(f"DEPS line {node.lineno}: cannot read {ast.dump(node)[:80]}")

    for statement in ast.parse(text).body:
        if isinstance(statement, ast.Assign) and isinstance(statement.targets[0], ast.Name):
            name = statement.targets[0].id
            if name in ("vars", "deps"):
                scope[name] = value(statement.value)
    return scope


def sha256_of_cipd_instance(instance_id: str) -> str:
    """Returns the hash in a CIPD instance ID: base64 of the hash and its algorithm's number."""
    raw = base64.urlsafe_b64decode(instance_id + "=" * (-len(instance_id) % 4))
    # 2 is SHA-256.
    if len(raw) != 33 or raw[-1] != 2:
        die(f"CIPD instance ID {instance_id} does not hold a SHA-256")
    return raw[:-1].hex()


def toolchain_pins(
    deps: Mapping[str, Any], host: Host, resolve_cipd: Callable[[str, str], str]
) -> list[dict[str, str]]:
    """Returns where this Host's toolchain lies, as pinned by a release's DEPS."""
    pins: list[dict[str, str]] = []
    for package in host.packages:
        entry = deps["deps"].get(f"src/{package.path}")
        if entry is None:
            die(f"DEPS has no entry src/{package.path}")
        match package:
            case GcsPackage(path, pattern):
                objects = [o for o in entry["objects"] if re.fullmatch(pattern, o["object_name"])]
                if len(objects) != 1:
                    die(f"DEPS has {len(objects)} objects matching {pattern} in src/{path}")
                url = f"https://storage.googleapis.com/{entry['bucket']}/{objects[0]['object_name']}"
                pins.append({"path": path, "url": url, "sha256": objects[0]["sha256sum"], "format": "tar"})
            case CipdPackage(path):
                name = (
                    entry["packages"][0]["package"]
                    .replace("${{platform}}", host.cipd_platform)
                    .replace("${{arch}}", host.cipd_arch)
                )
                instance_id = resolve_cipd(name, entry["packages"][0]["version"])
                url = f"https://chrome-infra-packages.appspot.com/dl/{name}/+/{instance_id}"
                pins.append({"path": path, "url": url, "sha256": sha256_of_cipd_instance(instance_id), "format": "zip"})
    return pins


def merge_slices(slices: Iterable[Path]) -> dict[str, Path]:
    """Returns the union of the slices' files. The same path must mean the same file."""
    merged: dict[str, Path] = {}
    for slice_dir in slices:
        root = slice_dir / "src"
        for file in _files_and_links(root):
            path = str(file.relative_to(root))
            if path not in merged:
                merged[path] = file
            elif not _same_file(merged[path], file) and path not in PER_HOST_FILES:
                die(f"{path} differs between {merged[path]} and {file}")
    return merged


def _files_and_links(root: Path) -> Iterator[Path]:
    for directory, subdirectories, names in os.walk(root):
        for name in [*names, *subdirectories]:
            path = Path(directory) / name
            if path.is_symlink() or path.is_file():
                yield path


def _same_file(first: Path, second: Path) -> bool:
    if first.is_symlink() or second.is_symlink():
        return first.is_symlink() and second.is_symlink() and first.readlink() == second.readlink()
    return filecmp.cmp(first, second, shallow=False)


def write_archive(
    destination: Path, root: str, files: Mapping[str, Path], extras: Mapping[str, bytes], mtime: int
) -> None:
    """Writes the archive. The same input gives the same bytes, on any machine.

    The files go under src/, and the extras beside it.
    """

    def entry(name: str, kind: bytes = tarfile.REGTYPE, mode: int = 0o644, size: int = 0) -> tarfile.TarInfo:
        info = tarfile.TarInfo(f"{root}/{name}")
        info.type, info.mode, info.size, info.mtime = kind, mode, size, mtime
        return info

    partial = destination.with_name(destination.name + ".partial")
    with (
        lzma.open(partial, "wb", preset=9 | lzma.PRESET_EXTREME) as compressed,
        tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as tar,
    ):
        for name in sorted(extras):
            tar.addfile(entry(name, size=len(extras[name])), io.BytesIO(extras[name]))
        for name in sorted(files):
            file = files[name]
            if file.is_symlink():
                link = entry(f"src/{name}", kind=tarfile.SYMTYPE)
                link.linkname = str(file.readlink())
                tar.addfile(link)
            else:
                stats = file.stat()
                mode = 0o755 if stats.st_mode & stat.S_IXUSR else 0o644
                with file.open("rb") as content:
                    tar.addfile(entry(f"src/{name}", mode=mode, size=stats.st_size), content)
    partial.replace(destination)


def resolve_with_depot_tools(workspace: Path) -> Callable[[str, str], str]:
    cipd = workspace / "depot_tools" / "cipd"

    def resolve(package: str, version: str) -> str:
        with tempfile.TemporaryDirectory() as temporary:
            result = Path(temporary) / "resolved.json"
            subprocess.run(
                [cipd, "resolve", package, "-version", version, "-json-output", result], check=True, capture_output=True
            )
            return json.loads(result.read_text())["result"][""][0]["pin"]["instance_id"]

    return resolve


def summarize(files: Mapping[str, Path]) -> list[tuple[str, int, int]]:
    """Returns (top-level directory, files, bytes), largest first."""
    counts: Counter[str] = Counter()
    sizes: Counter[str] = Counter()
    for name, file in files.items():
        top = name.split("/", 1)[0] if "/" in name else "(top level)"
        counts[top] += 1
        sizes[top] += file.lstat().st_size
    return [(top, counts[top], size) for top, size in sizes.most_common()]


def make_archive(workspace: Path, tag: str) -> Path:
    directory = export_dir(workspace, tag)
    infos: list[dict[str, Any]] = []
    for host in HOSTS:
        info_file = directory / host.name / "slice.json"
        if not info_file.is_file():
            die(f"{info_file} is missing; run this script's slice command on that Host and copy its result here")
        infos.append(json.loads(info_file.read_text()))
    patches = {patch.name: sha256_of(patch) for patch in sorted(PATCHES.glob("*.patch"))}
    for info in infos:
        if info["chromium_tag"] != tag or info["patches"] != patches:
            die(f"the {info['host']} slice is from another release or other patches; make it again")
    if len({info["commit_time"] for info in infos}) != 1:
        die("the slices come from different commits")

    files = merge_slices(directory / host.name for host in HOSTS)
    missing = third_party_without_licence(files)
    if missing:
        die("no licence file found for " + ", ".join(missing))
    deps = evaluate_deps(files["DEPS"].read_text())
    resolve = resolve_with_depot_tools(workspace)
    metadata = {
        "chromium_tag": tag,
        "patches": patches,
        "traced_libraries": {info["host"]: info["libraries"] for info in infos},
        "toolchain": {host.name: toolchain_pins(deps, host, resolve) for host in HOSTS},
    }
    extras = {
        "README.md": ARCHIVE_README.format(tag=tag).encode(),
        "export.json": (json.dumps(metadata, indent=2) + "\n").encode(),
    }
    extras |= {f"patches/{patch.name}": patch.read_bytes() for patch in sorted(PATCHES.glob("*.patch"))}
    destination = directory / archive_name(tag)
    note(f"compressing {len(files)} files; this takes minutes")
    write_archive(destination, f"chromium-{tag}", files, extras, infos[0]["commit_time"])
    size = destination.stat().st_size
    if size > ASSET_LIMIT:
        die(f"{destination} is {human_size(size)}, over GitHub's limit for a release asset")
    print(f"{destination}\n  {human_size(size)}, {len(files)} files, SHA-256 {sha256_of(destination)}")
    for top, count, total in summarize(files):
        print(f"  {top:<24} {count:>6} files {human_size(total):>12}")
    return destination


# Building from the export, with no Chromium checkout.


def download(url: str, sha256: str, directory: Path) -> Path:
    """Fetches a file, unless it is already here, and checks its hash."""
    directory.mkdir(parents=True, exist_ok=True)
    file = directory / sha256
    if file.is_file() and sha256_of(file) == sha256:
        return file
    note(f"downloading {url}")
    # A connection that drops resumes where it stopped, in this run or the next.
    retries = ["--retry", "5", "--retry-all-errors", "--continue-at", "-"]
    check_call(["curl", "--fail", "--location", "--silent", "--show-error", *retries, "--output", file, url])
    actual = sha256_of(file)
    if actual != sha256:
        file.unlink()
        die(f"{url} has SHA-256 {actual}, not {sha256}")
    return file


def extract_zip(archive: Path, destination: Path) -> None:
    """Unpacks a CIPD package. zipfile drops file modes and symlinks, so restore them.

    zipfile also gives each file the current time. The build depends on some of these
    programs, so ninja would take a package unpacked after a build's objects for a
    change, and rebuild what depends on it. A fixed old time keeps those objects valid.
    """
    with zipfile.ZipFile(archive) as packed:
        for member in packed.infolist():
            if member.filename.startswith(".cipdpkg/"):
                continue
            target = destination / member.filename
            mode = member.external_attr >> 16
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.is_dir():
                target.mkdir(exist_ok=True)
            elif stat.S_ISLNK(mode):
                target.unlink(missing_ok=True)
                target.symlink_to(packed.read(member).decode())
            else:
                target.write_bytes(packed.read(member))
                target.chmod(0o755 if mode & stat.S_IXUSR else 0o644)
    for path in destination.rglob("*"):
        if not path.is_symlink():
            os.utime(path, (CIPD_FILE_TIME, CIPD_FILE_TIME))


def fetch_toolchain(metadata: Mapping[str, Any], host: Host, checkout: Path, downloads: Path) -> None:
    """Puts this Host's toolchain in a checkout. Checkouts can share their downloads."""
    stamps = checkout / "toolchain-unpacked"
    stamps.mkdir(exist_ok=True)
    for pin in metadata["toolchain"][host.name]:
        stamp = stamps / pin["sha256"]
        if stamp.is_file():
            continue
        file = download(pin["url"], pin["sha256"], downloads)
        destination = checkout / "src" / pin["path"]
        destination.mkdir(parents=True, exist_ok=True)
        if pin["format"] == "zip":
            extract_zip(file, destination)
        else:
            check_call(["tar", "-xf", file, "-C", destination])
        stamp.touch()


def check_xcode() -> None:
    developer_dir = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True).stdout.strip()
    if not (Path(developer_dir) / "Platforms/MacOSX.platform/Developer/SDKs").is_dir():
        die(
            f"gn gen needs a full Xcode installation and xcode-select points to '{developer_dir}'; see docs/building.md"
        )


def archive_from_pin(directory: Path) -> Path:
    if not PIN_FILE.is_file():
        die(f"{PIN_FILE} is missing; name an archive with --archive")
    pin = json.loads(PIN_FILE.read_text())
    return download(pin["url"], pin["sha256"], directory / "downloads")


def unpack_export(archive: Path, checkout: Path) -> None:
    """Unpacks an archive as the checkout, unless that checkout already is that archive.

    A checkout only appears once it is whole, so a run that stopped starts over.
    An export replaced under the same release must not build with the old sources.
    A checkout with no stamp holds only `src/out`, restored from a cache of a build's
    objects. Those move into the new checkout, so that the build can reuse them.
    """
    identity = sha256_of(archive)
    stamp = checkout / "archive.sha256"
    if stamp.is_file():
        if stamp.read_text().strip() != identity:
            die(f"{checkout} holds another export of this release; use a new directory")
        return
    note(f"unpacking {archive}")
    with tempfile.TemporaryDirectory(dir=checkout.parent, prefix=".unpacking-") as temporary:
        check_call(["tar", "-xf", archive, "-C", temporary])
        unpacked = Path(temporary) / checkout.name
        if not (unpacked / "export.json").is_file():
            die(f"{archive} has no {checkout.name}/export.json; it is not the export of that release")
        (unpacked / stamp.name).write_text(identity + "\n")
        if checkout.exists():
            (checkout / "src/out").rename(unpacked / "src/out")
            shutil.rmtree(checkout)
        unpacked.rename(checkout)


def build_from_export(directory: Path, archive: Path | None, config: BuildConfig = PLAIN) -> Path:
    tag = read_tag()
    host = current_host()
    if config not in host.configs:
        die(f"the {config.name} build is only traced on other Hosts, so the export may lack its files here")
    directory.mkdir(parents=True, exist_ok=True)
    directory = directory.resolve()
    if re.search(r"\s", str(directory)):
        die(f"{directory} has a space in it; Chromium does not support that")
    if host is MAC:
        check_xcode()

    checkout = directory / f"chromium-{tag}"
    unpack_export(archive.resolve() if archive else archive_from_pin(directory), checkout)
    metadata = json.loads((checkout / "export.json").read_text())
    fetch_toolchain(metadata, host, checkout, directory / "downloads")

    # What a pull request changes comes from the repository, not the export.
    src = checkout / "src"
    link = src / "mitto"
    link.unlink(missing_ok=True)
    link.symlink_to(NATIVE)
    relative = Path("out") / config.name
    build_dir = src / relative
    build_dir.mkdir(parents=True, exist_ok=True)
    write_args(config, build_dir)

    env = build_environment()
    # The check takes seconds, and stops the build before it compiles anything.
    check_call([src / host.gn, "gen", relative, *GN_ROOT, "--check"], cwd=src, env=env)
    check_call([src / "third_party/ninja/ninja", "-C", relative, "mitto"], cwd=src, env=env)
    library = build_dir / host.library
    if library_fingerprint(library) == metadata["traced_libraries"][host.name][config.name]:
        note("the library is the same as the one the slice of this export built")
    else:
        note("the library differs from the one the slice of this export built, as it should once native/ has changed")
    print(library)
    return library


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("slice", help="trace a build in this Host's checkout and keep what it used")
    commands.add_parser("merge", help="merge every Host's slice into the export")
    build = commands.add_parser("build", help="build the native core from the export")
    build.add_argument("directory", type=Path, help="where to unpack and build")
    build.add_argument("--archive", type=Path, help="a local archive, instead of the published export")
    build.add_argument(
        "--config", choices=[config.name for config in CONFIGS], default=PLAIN.name, help="what kind of build to make"
    )
    arguments = parser.parse_args()
    try:
        match arguments.command:
            case "slice":
                make_slice(workspace_dir(), read_tag(), current_host())
            case "merge":
                make_archive(workspace_dir(), read_tag())
            case _:
                config = next(config for config in CONFIGS if config.name == arguments.config)
                build_from_export(arguments.directory, arguments.archive, config)
    except ExportError as error:
        print(f"export: error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"export: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
