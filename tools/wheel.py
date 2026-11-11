"""Puts the native core into mitto's wheel, as hatchling's build hook.

A wheel of mitto is for one Host and holds the native library next to the loader in src/mitto/_native.py. Name the
library in MITTO_WHEEL_LIBRARY and build as usual:

    MITTO_WHEEL_LIBRARY=/path/to/libmitto_core.dylib uv build --wheel

The hook strips a copy of the library and keeps what it removed in an archive next to the wheel, named after it. A
Windows DLL holds no debug information to strip: it names a PDB next to it, which goes into the archive instead. The
wheel's tag is py3-none-PLATFORM, with the platform taken from the library itself: the macOS version it needs, the
highest glibc version it needs, as auditwheel names it, or the CPU of a DLL. A wheel without a library would install
anywhere and fail on its first request, so the hook refuses to build one. An editable install needs none, because it
takes the library from MITTO_NATIVE_LIBRARY.

CI's tests also name the Runtime's Page program in MITTO_WHEEL_PAGE_PROGRAM. The hook then puts a stripped copy of it
next to the library, and what stripping removed into the same archive. Without the variable the wheel holds no Page
program, and the release workflow sets none, so that no release ships it before the Runtime does.

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
from collections.abc import Sequence
from pathlib import Path, PureWindowsPath
from typing import Any, NamedTuple

from hatchling.builders.config import BuilderConfig
from hatchling.builders.hooks.plugin.interface import BuildHookInterface

LIBRARY_ENV_VAR = "MITTO_WHEEL_LIBRARY"
PAGE_PROGRAM_ENV_VAR = "MITTO_WHEEL_PAGE_PROGRAM"

# Where the loader looks for the library in the installed package.
PACKAGE = "mitto"

SYMBOLS_SUFFIX = ".symbols.tar.xz"

MACOS_LIBRARY = "libmitto_core.dylib"
LINUX_LIBRARY = "libmitto_core.so"
WINDOWS_LIBRARY = "mitto_core.dll"
PAGE_PROGRAM = "mitto_page"
WINDOWS_PAGE_PROGRAM = "mitto_page.exe"

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

# A Windows program starts with an MS-DOS header, whose field at this offset says where the PE signature lies.
PE_MAGIC = b"MZ"
PE_OFFSET_FIELD = 0x3C
PE_SIGNATURE = b"PE\0\0"
PE_ARCHITECTURES = {0x8664: "amd64"}
PE_FILE_HEADER_SIZE = 20
PE32_PLUS = 0x20B
IMAGE_FILE_DLL = 0x2000
PE_DATA_DIRECTORIES_OFFSET = 112
PE_DEBUG_DIRECTORY = 6
PE_DEBUG_ENTRY_SIZE = 28
PE_DEBUG_TYPE_CODEVIEW = 2
PE_SECTION_HEADER_SIZE = 40
# A CodeView record names the PDB by its file name, its GUID and its age, which the PDB's info stream holds too.
CODEVIEW_PDB70 = b"RSDS"
PDB_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
PDB_INFO_STREAM = 1

# The glibc versions that auditwheel, which checks manylinux wheels, has a policy for. A tag can name any version, but
# auditwheel accepts no other, so a library that needs another version gets the next of these.
MANYLINUX_GLIBC_VERSIONS = (
    (2, 5),
    (2, 12),
    (2, 17),
    (2, 24),
    (2, 26),
    (2, 27),
    (2, 28),
    (2, 31),
    (2, 34),
    (2, 35),
    (2, 36),
    (2, 37),
    (2, 38),
    (2, 39),
    (2, 40),
    (2, 41),
)


class WheelError(Exception):
    """Stops the build with a message for the person running it."""


class _Names(NamedTuple):
    """What a binary is called in the wheel, by the format of the file: Mach-O, ELF or PE."""

    mach_o: str
    elf: str
    pe: str


_LIBRARY_NAMES = _Names(MACOS_LIBRARY, LINUX_LIBRARY, WINDOWS_LIBRARY)
_PAGE_PROGRAM_NAMES = _Names(PAGE_PROGRAM, PAGE_PROGRAM, WINDOWS_PAGE_PROGRAM)


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
    if data.startswith(PE_MAGIC):
        return _windows_tag(library, data)
    raise WheelError(f"{library} is neither a Mach-O, an ELF nor a PE file")


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
            major, minor, patch = minimum >> 16, (minimum >> 8) & 0xFF, minimum & 0xFF
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
    needed = max(versions)
    policy = next((version for version in MANYLINUX_GLIBC_VERSIONS if version >= needed), None)
    if policy is None:
        raise WheelError(f"{library} needs glibc {needed[0]}.{needed[1]}, which no manylinux policy covers")
    return f"manylinux_{policy[0]}_{policy[1]}_{ELF_ARCHITECTURES[machine]}"


def _windows_tag(library: Path, data: bytes) -> str:
    (pe_offset,) = struct.unpack_from("<I", data, PE_OFFSET_FIELD)
    if data[pe_offset : pe_offset + len(PE_SIGNATURE)] != PE_SIGNATURE:
        raise WheelError(f"{library} has an MS-DOS header but no PE signature")
    machine, characteristics = struct.unpack_from("<H16xH", data, pe_offset + 4)
    if machine not in PE_ARCHITECTURES:
        raise WheelError(f"{library} is for PE machine {machine:#x}, not for x64")
    if not characteristics & IMAGE_FILE_DLL:
        raise WheelError(f"{library} is a program, not a DLL")
    return f"win_{PE_ARCHITECTURES[machine]}"


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
        on macOS, and a file that the library links to by name on Linux. A DLL is only copied, and its PDB stays
        where the build put it.

    Raises:
        WheelError: If the file is not a library of a Host, or a tool fails, or a DLL's PDB is missing or another's.
    """
    return _strip(library, directory, _LIBRARY_NAMES)


def strip_page_program(program: Path, directory: Path) -> tuple[Path, Path]:
    """Copies the Page program into a directory and strips the copy, as strip_library does a library.

    Returns:
        The stripped program, named mitto_page or mitto_page.exe, and what stripping removed from it.

    Raises:
        WheelError: If the file is not a program of a Host, or a tool fails, or the program's PDB is missing or another's.
    """
    return _strip(program, directory, _PAGE_PROGRAM_NAMES)


def _strip(binary: Path, directory: Path, names: _Names) -> tuple[Path, Path]:
    with binary.open("rb") as file:
        magic = file.read(len(ELF_MAGIC))
    if magic == MACH_O_MAGIC:
        return _strip_mach_o(binary, directory / names.mach_o)
    if magic == ELF_MAGIC:
        return _strip_elf(binary, directory / names.elf)
    if magic.startswith(PE_MAGIC):
        return _copy_pe(binary, directory / names.pe)
    raise WheelError(f"{binary} is neither a Mach-O, an ELF nor a PE file")


def _strip_mach_o(binary: Path, stripped: Path) -> tuple[Path, Path]:
    symbols = stripped.with_name(f"{stripped.name}.dSYM")
    shutil.copy(binary, stripped)
    # A binary only points to the build's object files, which hold its debug information. Without them, such as for a
    # binary from a cache, dsymutil warns about each one, and the symbols keep function names but not line numbers.
    if warnings := _run("dsymutil", stripped, "-o", symbols).splitlines():
        print(f"wheel: dsymutil warned {len(warnings)} times, first: {warnings[0]}", file=sys.stderr)
    _run("strip", "-x", stripped)
    # macOS on arm64 does not load a library whose signature does not hold. strip signs again what the linker signed.
    _run("codesign", "--verify", "--strict", stripped)
    return stripped, symbols


def _strip_elf(binary: Path, stripped: Path) -> tuple[Path, Path]:
    symbols = stripped.with_name(f"{stripped.name}.debug")
    shutil.copy(binary, stripped)
    _run("objcopy", "--only-keep-debug", stripped, symbols)
    _run("strip", "--strip-all", stripped)
    # The link holds the file's name and checksum, so that a debugger finds it next to the binary and knows it.
    _run("objcopy", f"--add-gnu-debuglink={symbols}", stripped)
    return stripped, symbols


def _copy_pe(binary: Path, copy: Path) -> tuple[Path, Path]:
    shutil.copy(binary, copy)
    name, identity = codeview_record(copy)
    # The linker names the PDB by its path, or by its file name alone, as Chromium's build asks with /pdbaltpath.
    symbols = binary.parent / PureWindowsPath(name).name
    if not symbols.is_file():
        raise WheelError(f"{binary} names its PDB {name}, and {symbols} is missing")
    if pdb_identity(symbols) != identity:
        raise WheelError(f"{symbols} is not the PDB of {binary}, but one of another build")
    return copy, symbols


def codeview_record(dll: Path) -> tuple[str, bytes]:
    """Returns what a DLL's CodeView debug record says of its PDB: its path, and its GUID followed by its age.

    Raises:
        WheelError: If the DLL is not 64-bit or has no such record.
    """
    data = dll.read_bytes()
    (pe_offset,) = struct.unpack_from("<I", data, PE_OFFSET_FIELD)
    section_count, optional_size = struct.unpack_from("<H12xH", data, pe_offset + 6)
    optional = pe_offset + len(PE_SIGNATURE) + PE_FILE_HEADER_SIZE
    (magic,) = struct.unpack_from("<H", data, optional)
    if magic != PE32_PLUS:
        raise WheelError(f"{dll} is not a 64-bit PE file")
    directory = optional + PE_DATA_DIRECTORIES_OFFSET + 8 * PE_DEBUG_DIRECTORY
    debug_address, debug_size = struct.unpack_from("<2I", data, directory)
    for index in range(section_count):
        virtual_size, address, _, raw_offset = struct.unpack_from(
            "<4I", data, optional + optional_size + index * PE_SECTION_HEADER_SIZE + 8
        )
        if not address <= debug_address < address + virtual_size:
            continue
        start = raw_offset + debug_address - address
        for entry in range(start, start + debug_size, PE_DEBUG_ENTRY_SIZE):
            kind, size, _, record = struct.unpack_from("<4I", data, entry + 12)
            if kind == PE_DEBUG_TYPE_CODEVIEW and data[record : record + len(CODEVIEW_PDB70)] == CODEVIEW_PDB70:
                body = data[record + len(CODEVIEW_PDB70) : record + size]
                return body[20 : body.index(b"\0", 20)].decode(), body[:20]
    raise WheelError(f"{dll} names no PDB; it must be linked with /DEBUG")


def pdb_identity(pdb: Path) -> bytes:
    """Returns the GUID and the age of a PDB, from its info stream, as a CodeView record names them.

    A PDB is an MSF file: fixed-size blocks, a directory of streams, each stream a list of blocks.

    Raises:
        WheelError: If the file is not a PDB.
    """
    with pdb.open("rb") as file:
        header = file.read(len(PDB_MAGIC) + 24)
        if not header.startswith(PDB_MAGIC):
            raise WheelError(f"{pdb} is not a PDB")
        block_size, _, _, directory_size, _, block_map = struct.unpack_from("<6I", header, len(PDB_MAGIC))

        def read_blocks(blocks: list[int], size: int) -> bytes:
            parts: list[bytes] = []
            for block in blocks:
                file.seek(block * block_size)
                parts.append(file.read(block_size))
            return b"".join(parts)[:size]

        directory_block_count = -(-directory_size // block_size)
        file.seek(block_map * block_size)
        directory_blocks = list(struct.unpack(f"<{directory_block_count}I", file.read(4 * directory_block_count)))
        directory = read_blocks(directory_blocks, directory_size)
        (stream_count,) = struct.unpack_from("<I", directory)
        if stream_count <= PDB_INFO_STREAM:
            raise WheelError(f"{pdb} has no info stream")
        sizes = struct.unpack_from(f"<{stream_count}I", directory, 4)
        # A stream that does not exist has the size 0xFFFFFFFF and no blocks.
        block_counts = [0 if size == 0xFFFFFFFF else -(-size // block_size) for size in sizes]
        first = 4 + 4 * stream_count + 4 * sum(block_counts[:PDB_INFO_STREAM])
        blocks = list(struct.unpack_from(f"<{block_counts[PDB_INFO_STREAM]}I", directory, first))
        info = read_blocks(blocks, sizes[PDB_INFO_STREAM])
    # Version and time, then the age and the GUID, which a CodeView record holds in the other order.
    (age,) = struct.unpack_from("<I", info, 8)
    return info[12:28] + struct.pack("<I", age)


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


def write_symbols_archive(symbols: Sequence[Path], archive: Path) -> None:
    """Writes the files or bundles that stripping removed into one archive, each under its own name."""

    def without_owner(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    # A higher preset takes ten times as long for a tenth less.
    with tarfile.open(archive, "w:xz", preset=1) as tar:
        for path in symbols:
            tar.add(path, path.name, filter=without_owner)


def _file_named_in(variable: str) -> Path | None:
    """Returns the file that an environment variable names, or None if it is unset or empty.

    Raises:
        WheelError: If the variable names something that is not a file.
    """
    value = os.environ.get(variable)
    if not value:
        return None
    path = Path(value).resolve()
    if not path.is_file():
        raise WheelError(f"{variable} names {path}, which is not a file")
    return path


class NativeLibraryHook(BuildHookInterface[BuilderConfig]):
    """Puts the stripped native library into the wheel, tags the wheel for it and writes the symbols archive.

    It also puts in the stripped Page program, if MITTO_WHEEL_PAGE_PROGRAM names one.
    """

    PLUGIN_NAME = "custom"

    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        if version == "editable":
            return
        library = _file_named_in(LIBRARY_ENV_VAR)
        if library is None:
            raise WheelError(f"set {LIBRARY_ENV_VAR} to the native library; a wheel without it cannot make a request")
        program = _file_named_in(PAGE_PROGRAM_ENV_VAR)
        self._directory = Path(tempfile.mkdtemp(prefix="mitto-wheel-"))
        try:
            stripped, symbols = strip_library(library, self._directory)
            tag = f"py3-none-{platform_tag(stripped)}"
            binaries, self._symbols = [stripped], [symbols]
            if program is not None:
                stripped_program, program_symbols = strip_page_program(program, self._directory)
                binaries.append(stripped_program)
                self._symbols.append(program_symbols)
        except BaseException:
            shutil.rmtree(self._directory)
            raise
        for binary in binaries:
            build_data["force_include"][str(binary)] = f"{PACKAGE}/{binary.name}"
        build_data["pure_python"] = False
        build_data["tag"] = tag

    def finalize(self, version: str, build_data: dict[str, Any], artifact_path: str) -> None:
        if version == "editable":
            return
        write_symbols_archive(self._symbols, symbols_archive_of(Path(artifact_path)))
        shutil.rmtree(self._directory)
