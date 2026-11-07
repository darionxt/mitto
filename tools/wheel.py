"""Puts the native core into mitto's wheel, as hatchling's build hook.

A wheel of mitto is for one Host and holds the native library next to the loader in src/mitto/_native.py. Name the
library in MITTO_WHEEL_LIBRARY and build as usual:

    MITTO_WHEEL_LIBRARY=/path/to/libmitto_core.dylib uv build --wheel

The hook strips a copy of the library and keeps what it removed in an archive next to the wheel, named after it. The
wheel's tag is py3-none-PLATFORM, with the platform taken from the library itself: the macOS version it needs, or the
highest glibc version it needs. A wheel without a library would install anywhere and fail on its first request, so the
hook refuses to build one. An editable install needs none, because it takes the library from MITTO_NATIVE_LIBRARY.

Hatchling loads this file by its path, outside the project's environment, so it imports nothing but the standard library
and hatchling. See docs/building.md.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import Any

from hatchling.builders.config import BuilderConfig
from hatchling.builders.hooks.plugin.interface import BuildHookInterface

LIBRARY_ENV_VAR = "MITTO_WHEEL_LIBRARY"

# Where the loader looks for the library in the installed package.
PACKAGE = "mitto"

SYMBOLS_SUFFIX = ".symbols.tar.xz"

MACOS_LIBRARY = "libmitto_core.dylib"
LINUX_LIBRARY = "libmitto_core.so"

MACH_O_MAGIC = b"\xcf\xfa\xed\xfe"
MACH_O_HEADER_SIZE = 32
MACH_O_ARCHITECTURES = {0x0100000C: "arm64"}
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1

ELF_MAGIC = b"\x7fELF"
ELF_64_LITTLE_ENDIAN = b"\x02\x01"
ELF_ARCHITECTURES = {62: "x86_64"}
SHT_GNU_VERNEED = 0x6FFFFFFE
GLIBC_VERSION = re.compile(r"GLIBC_(\d+)\.(\d+)(\.\d+)?")


class WheelError(Exception):
    """Stops the build with a message for the person running it."""


def platform_tag(library: Path) -> str:
    """Returns the platform part of the wheel tag that a library needs, such as macosx_13_0_arm64.

    Raises:
        WheelError: If the file is not a library of a Host, or what it needs has no tag.
    """
    data = library.read_bytes()
    if data.startswith(MACH_O_MAGIC):
        return _macos_tag(library, data)
    if data.startswith(ELF_MAGIC):
        return _manylinux_tag(library, data)
    raise WheelError(f"{library} is neither a Mach-O nor an ELF file")


def _macos_tag(library: Path, data: bytes) -> str:
    (cpu_type,) = struct.unpack_from("<I", data, 4)
    if cpu_type not in MACH_O_ARCHITECTURES:
        raise WheelError(f"{library} is for CPU type {cpu_type:#x}, not for arm64")
    (command_count,) = struct.unpack_from("<I", data, 16)
    command = MACH_O_HEADER_SIZE
    for _ in range(command_count):
        kind, command_size = struct.unpack_from("<2I", data, command)
        if kind == LC_BUILD_VERSION:
            platform, minimum = struct.unpack_from("<2I", data, command + 8)
            if platform != PLATFORM_MACOS:
                raise WheelError(f"{library} is for Apple platform {platform}, not for macOS")
            major, minor, patch = minimum >> 16, minimum >> 8 & 0xFF, minimum & 0xFF
            # From macOS 11 on, pip only matches tags of a major version, such as macosx_13_0.
            if minor or patch:
                raise WheelError(f"{library} needs macOS {major}.{minor}.{patch}, and a tag can only say {major}.0")
            return f"macosx_{major}_0_{MACH_O_ARCHITECTURES[cpu_type]}"
        command += command_size
    raise WheelError(f"{library} does not say which macOS version it needs")


def _manylinux_tag(library: Path, data: bytes) -> str:
    if data[4:6] != ELF_64_LITTLE_ENDIAN:
        raise WheelError(f"{library} is not a 64-bit little-endian ELF file")
    (machine,) = struct.unpack_from("<H", data, 18)
    if machine not in ELF_ARCHITECTURES:
        raise WheelError(f"{library} is for ELF machine {machine}, not for x86_64")
    versions: list[tuple[int, int]] = []
    for name in _needed_versions(data):
        if not name.startswith("GLIBC_"):
            continue
        match = GLIBC_VERSION.fullmatch(name)
        if match is None:
            raise WheelError(f"{library} needs {name} from glibc, which no manylinux tag allows")
        versions.append((int(match[1]), int(match[2])))
    if not versions:
        raise WheelError(f"{library} needs no version of glibc")
    major, minor = max(versions)
    return f"manylinux_{major}_{minor}_{ELF_ARCHITECTURES[machine]}"


def _needed_versions(data: bytes) -> set[str]:
    """Returns the symbol versions that an ELF file needs from other libraries, such as GLIBC_2.17."""
    (section_headers,) = struct.unpack_from("<Q", data, 0x28)
    header_size, header_count = struct.unpack_from("<2H", data, 0x3A)
    sections = [struct.unpack_from("<2I4Q2I2Q", data, section_headers + i * header_size) for i in range(header_count)]
    names: set[str] = set()
    for section in sections:
        kind, offset, strings_index, entry_count = section[1], section[4], section[6], section[7]
        if kind != SHT_GNU_VERNEED:
            continue
        strings = sections[strings_index][4]
        entry = offset
        for _ in range(entry_count):
            _, auxiliary_count, _, auxiliary_offset, next_entry = struct.unpack_from("<2H3I", data, entry)
            auxiliary = entry + auxiliary_offset
            for _ in range(auxiliary_count):
                _, _, _, name, next_auxiliary = struct.unpack_from("<I2H2I", data, auxiliary)
                end = data.index(b"\0", strings + name)
                names.add(data[strings + name : end].decode())
                auxiliary += next_auxiliary
            entry += next_entry
    return names


def strip_library(library: Path, directory: Path) -> tuple[Path, Path]:
    """Copies a library into a directory and strips the copy.

    Returns:
        The stripped library, under the name the loader looks for, and what stripping removed from it: a dSYM bundle
        on macOS, and a file that the library links to by name on Linux.

    Raises:
        WheelError: If the file is not a library of a Host, or a tool fails.
    """
    with library.open("rb") as file:
        magic = file.read(len(ELF_MAGIC))
    if magic == MACH_O_MAGIC:
        return _strip_mach_o(library, directory)
    if magic == ELF_MAGIC:
        return _strip_elf(library, directory)
    raise WheelError(f"{library} is neither a Mach-O nor an ELF file")


def _strip_mach_o(library: Path, directory: Path) -> tuple[Path, Path]:
    stripped = directory / MACOS_LIBRARY
    symbols = directory / f"{MACOS_LIBRARY}.dSYM"
    shutil.copy(library, stripped)
    # The library only points to the build's object files, which hold its debug information. Without them, such as for
    # a library from a cache, dsymutil warns about each one, and the symbols keep function names but not line numbers.
    if warnings := _run("dsymutil", stripped, "-o", symbols).splitlines():
        print(f"wheel: dsymutil warned {len(warnings)} times, first: {warnings[0]}", file=sys.stderr)
    _run("strip", "-x", stripped)
    # macOS on arm64 does not load a library whose signature does not hold. strip signs again what the linker signed.
    _run("codesign", "--verify", "--strict", stripped)
    return stripped, symbols


def _strip_elf(library: Path, directory: Path) -> tuple[Path, Path]:
    stripped = directory / LINUX_LIBRARY
    symbols = directory / f"{LINUX_LIBRARY}.debug"
    shutil.copy(library, stripped)
    _run("objcopy", "--only-keep-debug", stripped, symbols)
    _run("strip", "--strip-all", stripped)
    # The link holds the file's name and checksum, so that a debugger finds it next to the library and knows it.
    _run("objcopy", f"--add-gnu-debuglink={symbols}", stripped)
    return stripped, symbols


def _run(*command: str | Path) -> str:
    """Runs a tool and returns what it wrote to stderr."""
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True).stderr
    except FileNotFoundError as error:
        raise WheelError(f"building a wheel needs {command[0]}; see docs/building.md") from error
    except subprocess.CalledProcessError as error:
        raise WheelError(
            f"{command[0]} failed with status {error.returncode}:\n{error.stdout}{error.stderr}"
        ) from error


def symbols_archive_of(wheel: Path) -> Path:
    """Returns the path of the archive that holds the symbols of a wheel's library: next to it, and named after it."""
    return wheel.with_name(wheel.name.removesuffix(".whl") + SYMBOLS_SUFFIX)


def write_symbols_archive(symbols: Path, archive: Path) -> None:
    def without_owner(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    # A higher preset takes ten times as long for a tenth less.
    with tarfile.open(archive, "w:xz", preset=1) as tar:
        tar.add(symbols, symbols.name, filter=without_owner)


class NativeLibraryHook(BuildHookInterface[BuilderConfig]):
    """Puts the stripped native library into the wheel, tags the wheel for it and writes the symbols archive."""

    PLUGIN_NAME = "custom"

    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        if version == "editable":
            return
        value = os.environ.get(LIBRARY_ENV_VAR)
        if not value:
            raise WheelError(f"set {LIBRARY_ENV_VAR} to the native library; a wheel without it cannot make a request")
        library = Path(value).resolve()
        if not library.is_file():
            raise WheelError(f"{LIBRARY_ENV_VAR} names {library}, which is not a file")
        self._directory = Path(tempfile.mkdtemp(prefix="mitto-wheel-"))
        stripped, self._symbols = strip_library(library, self._directory)
        build_data["force_include"][str(stripped)] = f"{PACKAGE}/{stripped.name}"
        build_data["pure_python"] = False
        build_data["tag"] = f"py3-none-{platform_tag(stripped)}"

    def finalize(self, version: str, build_data: dict[str, Any], artifact_path: str) -> None:
        if version == "editable":
            return
        write_symbols_archive(self._symbols, symbols_archive_of(Path(artifact_path)))
        shutil.rmtree(self._directory)
