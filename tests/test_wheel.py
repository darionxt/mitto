"""Tests of the build hook that puts the native core into the wheel.

Tests of the tag read made-up headers. Tests of the build run hatchling on this repository. Those that need a library
compile a stub with cc, and are skipped when there is no C compiler and on Windows, whose DLL and PDB are made up instead.
Those that take the built_wheel fixture check a wheel of the real native core, named in MITTO_WHEEL, and the package
installed from it. The test of its notices also needs
what the library links, which `python -m tools.workspace.notices host` prints from the library's build, in
MITTO_HOST_NOTICES. A wheel of the real native core holds the Page program only where MITTO_PAGE_PROGRAM names the one
installed from it, as in CI. The release workflow names none, so its wheels must hold none.
"""

import ctypes
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import zipfile
import zlib
from collections.abc import Mapping, Sequence
from importlib import resources
from pathlib import Path

import pytest

from mitto import _native
from tests.pe_file import PeFile
from tools import wheel
from tools.workspace import export, notices

REPOSITORY = Path(__file__).parents[1]

# Names a wheel of the real native core, with its symbols archive next to it.
WHEEL_ENV_VAR = "MITTO_WHEEL"

# Names this Host's part of the notices, from the build of the wheel's library.
HOST_NOTICES_ENV_VAR = "MITTO_HOST_NOTICES"

# Names the Page program that the tests of tests/test_page_program.py run.
PAGE_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"

RELEASE_WORKFLOW = REPOSITORY / ".github" / "workflows" / "release.yml"

# A function of the native core that the C API does not export.
HIDDEN_FUNCTION = "mitto::ParseSessionSettings"

STUB = """
#include <stdint.h>
#include <unistd.h>

__attribute__((visibility("hidden"))) int mitto_stub_hidden(int value) { return value * 3 + getpid() % 2; }

uint32_t mitto_abi_version(void) { return (uint32_t)mitto_stub_hidden(1); }
"""
STUB_HIDDEN_FUNCTION = "mitto_stub_hidden"
STUB_MACOS_VERSION = "13.0"

STUB_PROGRAM = """
#include <stdio.h>
#include <unistd.h>

__attribute__((visibility("hidden"))) int mitto_stub_page_hidden(int value) { return value * 7 + getpid() % 2; }

int main(void) {
  printf("%d\\n", mitto_stub_page_hidden(6) - getpid() % 2);
  return 0;
}
"""
STUB_PROGRAM_HIDDEN_FUNCTION = "mitto_stub_page_hidden"

CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007
PLATFORM_IOS = 2
EM_AARCH64 = 183
SHT_STRTAB = 3

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_FILE_EXECUTABLE_IMAGE = 0x2
IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x20
PE_OPTIONAL_HEADER_SIZE = 240
PE_SECTION_ADDRESS = 0x1000
PE_SECTION_OFFSET = 0x200
PDB_BLOCK_SIZE = 512
PDB_VERSION = 20000404
PDB_GUID = bytes(range(16))
PDB_NAME = "mitto_core.dll.pdb"
PAGE_PROGRAM_PDB_GUID = bytes(range(16, 32))
PAGE_PROGRAM_PDB_NAME = "mitto_page.exe.pdb"


def mach_o_library(
    cpu_type: int = CPU_TYPE_ARM64, platform: int = wheel.PLATFORM_MACOS, minimum: tuple[int, int, int] = (13, 0, 0)
) -> bytes:
    """Return the start of a Mach-O library, with only what its tag is read from."""
    major, minor, patch = minimum
    build_version = struct.pack("<6I", wheel.LC_BUILD_VERSION, 24, platform, major << 16 | minor << 8 | patch, 0, 0)
    header = struct.pack("<8I", 0xFEEDFACF, cpu_type, 0, 6, 1, len(build_version), 0, 0)
    return header + build_version


def elf_library(needs: Mapping[str, Sequence[str]], machine: int = 62) -> bytes:
    """Return an ELF library with only what its tag is read from: its machine, and the versions it needs by library."""
    strings = bytearray(b"\0")

    def string(text: str) -> int:
        strings.extend(text.encode() + b"\0")
        return len(strings) - len(text) - 1

    needed = bytearray()
    for index, (library, versions) in enumerate(needs.items()):
        next_library = 0 if index == len(needs) - 1 else 16 + 16 * len(versions)
        needed += struct.pack("<2H3I", 1, len(versions), string(library), 16, next_library)
        for position, version in enumerate(versions):
            next_version = 0 if position == len(versions) - 1 else 16
            needed += struct.pack("<I2H2I", 0, 0, 0, string(version), next_version)

    strings_offset = 64
    needed_offset = strings_offset + len(strings)
    sections_offset = needed_offset + len(needed)
    sections = [
        bytes(64),
        struct.pack("<2I4Q2I2Q", 0, SHT_STRTAB, 0, 0, strings_offset, len(strings), 0, 0, 1, 0),
        struct.pack("<2I4Q2I2Q", 0, wheel.SHT_GNU_VERNEED, 0, 0, needed_offset, len(needed), 1, len(needs), 4, 0),
    ]
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack(
        "<16s2HI3QI6H", identity, 3, machine, 1, 0, 0, sections_offset, 0, 64, 0, 0, 64, len(sections), 0
    )
    return header + strings + needed + b"".join(sections)


def pe_library(
    machine: int = IMAGE_FILE_MACHINE_AMD64,
    dll: bool = True,
    pdb: str | None = PDB_NAME,
    guid: bytes = PDB_GUID,
    age: int = 1,
) -> bytes:
    """Return a 64-bit PE file with only what the hook reads: its CPU, whether it is a DLL, and the PDB it names."""
    record = b"RSDS" + guid + struct.pack("<I", age) + (pdb or "").encode() + b"\0"
    debug_entry_size = 28
    section = (
        struct.pack(
            "<2I2H4I",
            0,
            0,
            0,
            0,
            wheel.PE_DEBUG_TYPE_CODEVIEW,
            len(record),
            PE_SECTION_ADDRESS + debug_entry_size,
            PE_SECTION_OFFSET + debug_entry_size,
        )
        + record
    )
    characteristics = (
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | (wheel.IMAGE_FILE_DLL if dll else 0)
    )
    dos_header = b"MZ" + bytes(0x3A) + struct.pack("<I", 64)
    file_header = struct.pack("<2H3I2H", machine, 1, 0, 0, 0, PE_OPTIONAL_HEADER_SIZE, characteristics)
    directories = bytearray(8 * 16)
    if pdb is not None:
        struct.pack_into("<2I", directories, 8 * wheel.PE_DEBUG_DIRECTORY, PE_SECTION_ADDRESS, debug_entry_size)
    optional_header = struct.pack("<H", wheel.PE32_PLUS).ljust(112, b"\0") + directories
    section_header = b".rdata\0\0" + struct.pack(
        "<4I", len(section), PE_SECTION_ADDRESS, len(section), PE_SECTION_OFFSET
    ).ljust(32, b"\0")
    headers = dos_header + b"PE\0\0" + file_header + optional_header + section_header
    return headers.ljust(PE_SECTION_OFFSET, b"\0") + section


def pe_page_program() -> bytes:
    """Return a made-up Page program for Windows, which names a PDB of its own."""
    return pe_library(dll=False, pdb=PAGE_PROGRAM_PDB_NAME, guid=PAGE_PROGRAM_PDB_GUID)


def pdb_file(guid: bytes = PDB_GUID, age: int = 1) -> bytes:
    """Return a PDB with only what the hook reads: an MSF file whose info stream holds a GUID and an age.

    Block 0 is the header, 1 the free block map, 2 the list of the directory's blocks, 3 the directory and 4 the info
    stream. Stream 0 does not exist, as its size of 0xFFFFFFFF says.
    """
    info = struct.pack("<3I", PDB_VERSION, 0, age) + guid
    directory = struct.pack("<4I", 2, 0xFFFFFFFF, len(info), 4)
    header = wheel.PDB_MAGIC + struct.pack("<6I", PDB_BLOCK_SIZE, 1, 5, len(directory), 0, 2)
    blocks = [header, b"", struct.pack("<I", 3), directory, info]
    return b"".join(block.ljust(PDB_BLOCK_SIZE, b"\0") for block in blocks)


def write(directory: Path, data: bytes) -> Path:
    library = directory / "library"
    library.write_bytes(data)
    return library


def run(command: Sequence[str | Path]) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def test_the_macos_tag_is_the_version_the_library_needs(tmp_path: Path) -> None:
    library = write(tmp_path, mach_o_library(minimum=(13, 0, 0)))

    assert wheel.platform_tag(library) == "macosx_13_0_arm64"


@pytest.mark.parametrize("minimum", [(13, 3, 0), (13, 0, 1)])
def test_a_macos_version_inside_a_major_version_has_no_tag(tmp_path: Path, minimum: tuple[int, int, int]) -> None:
    library = write(tmp_path, mach_o_library(minimum=minimum))

    with pytest.raises(wheel.WheelError, match=r"a tag can only say 13\.0"):
        wheel.platform_tag(library)


def test_a_library_for_another_apple_platform_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, mach_o_library(platform=PLATFORM_IOS))

    with pytest.raises(wheel.WheelError, match="not for macOS"):
        wheel.platform_tag(library)


def test_a_mach_o_library_for_another_cpu_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, mach_o_library(cpu_type=CPU_TYPE_X86_64))

    with pytest.raises(wheel.WheelError, match="not for arm64"):
        wheel.platform_tag(library)


def test_the_linux_tag_is_the_highest_glibc_version_the_library_needs(tmp_path: Path) -> None:
    needs = {
        "libc.so.6": ["GLIBC_2.2.5", "GLIBC_2.26", "GLIBC_2.9"],
        "libm.so.6": ["GLIBC_2.2.5"],
        # Versions of other libraries do not count, even when they are higher.
        "libgcc_s.so.1": ["GCC_3.0", "GCC_4.2.0"],
    }
    library = write(tmp_path, elf_library(needs))

    assert wheel.platform_tag(library) == "manylinux_2_26_x86_64"


@pytest.mark.parametrize(
    ("needed", "tag"),
    [("GLIBC_2.2.5", "manylinux_2_5_x86_64"), ("GLIBC_2.25", "manylinux_2_26_x86_64")],
)
def test_a_glibc_version_without_a_policy_gets_the_next_one(tmp_path: Path, needed: str, tag: str) -> None:
    library = write(tmp_path, elf_library({"libc.so.6": [needed]}))

    assert wheel.platform_tag(library) == tag


def test_a_glibc_version_newer_than_every_policy_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, elf_library({"libc.so.6": ["GLIBC_2.99"]}))

    with pytest.raises(wheel.WheelError, match=r"glibc 2\.99, which no manylinux policy covers"):
        wheel.platform_tag(library)


def test_the_glibc_versions_are_those_auditwheel_has_a_policy_for() -> None:
    pytest.importorskip("auditwheel", reason="auditwheel is only installed on Linux")
    policies = json.loads(resources.files("auditwheel.policy").joinpath("manylinux-policy.json").read_text())
    names = {policy["name"] for policy in policies} - {"linux"}

    assert names == {f"manylinux_{major}_{minor}" for major, minor in wheel.MANYLINUX_GLIBC_VERSIONS}


def test_a_private_glibc_version_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, elf_library({"libc.so.6": ["GLIBC_2.17", "GLIBC_PRIVATE"]}))

    with pytest.raises(wheel.WheelError, match="GLIBC_PRIVATE"):
        wheel.platform_tag(library)


def test_a_library_that_needs_no_glibc_version_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, elf_library({"libgcc_s.so.1": ["GCC_3.0"]}))

    with pytest.raises(wheel.WheelError, match="no version of glibc"):
        wheel.platform_tag(library)


def test_an_elf_library_for_another_cpu_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, elf_library({"libc.so.6": ["GLIBC_2.17"]}, machine=EM_AARCH64))

    with pytest.raises(wheel.WheelError, match="not for x86_64"):
        wheel.platform_tag(library)


def test_the_windows_tag_is_the_cpu_of_the_dll(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library())

    assert wheel.platform_tag(library) == "win_amd64"


def test_a_dll_for_another_cpu_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library(machine=IMAGE_FILE_MACHINE_ARM64))

    with pytest.raises(wheel.WheelError, match="not for x64"):
        wheel.platform_tag(library)


def test_a_windows_program_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library(dll=False))

    with pytest.raises(wheel.WheelError, match="a program, not a DLL"):
        wheel.platform_tag(library)


def test_a_file_that_is_not_a_library_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, b"#!/bin/sh\n")

    with pytest.raises(wheel.WheelError, match="neither a Mach-O, an ELF nor a PE file"):
        wheel.platform_tag(library)


@pytest.mark.parametrize("named", [PDB_NAME, rf"C:\build\out\mitto\{PDB_NAME}"])
def test_a_dll_goes_into_the_wheel_as_it_is_and_its_pdb_into_the_symbols(tmp_path: Path, named: str) -> None:
    built = tmp_path / "out"
    built.mkdir()
    library = built / "library.dll"
    library.write_bytes(pe_library(pdb=named))
    (built / PDB_NAME).write_bytes(pdb_file())
    copies = tmp_path / "copies"
    copies.mkdir()

    copy, symbols = wheel.strip_library(library, copies)

    assert copy == copies / wheel.WINDOWS_LIBRARY
    assert copy.read_bytes() == library.read_bytes()
    assert symbols == built / PDB_NAME


def test_a_dll_whose_pdb_is_missing_is_refused(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library())

    with pytest.raises(wheel.WheelError, match=f"names its PDB {PDB_NAME}, and .* is missing"):
        wheel.strip_library(library, tmp_path)


@pytest.mark.parametrize(("guid", "age"), [(PDB_GUID[::-1], 1), (PDB_GUID, 2)], ids=["guid", "age"])
def test_the_pdb_of_another_build_is_refused(tmp_path: Path, guid: bytes, age: int) -> None:
    library = write(tmp_path, pe_library())
    (tmp_path / PDB_NAME).write_bytes(pdb_file(guid, age))

    with pytest.raises(wheel.WheelError, match="is not the PDB of"):
        wheel.strip_library(library, tmp_path)


def test_a_dll_that_names_no_pdb_is_refused(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library(pdb=None))

    with pytest.raises(wheel.WheelError, match="names no PDB"):
        wheel.strip_library(library, tmp_path)


def test_the_page_program_for_windows_goes_into_the_wheel_as_it_is_and_its_pdb_into_the_symbols(tmp_path: Path) -> None:
    built = tmp_path / "out"
    built.mkdir()
    program = built / "program.exe"
    program.write_bytes(pe_page_program())
    (built / PAGE_PROGRAM_PDB_NAME).write_bytes(pdb_file(PAGE_PROGRAM_PDB_GUID))
    copies = tmp_path / "copies"
    copies.mkdir()

    copy, symbols = wheel.strip_page_program(program, copies)

    assert copy == copies / wheel.WINDOWS_PAGE_PROGRAM
    assert copy.read_bytes() == program.read_bytes()
    assert symbols == built / PAGE_PROGRAM_PDB_NAME


def test_a_page_program_that_is_no_program_is_refused(tmp_path: Path) -> None:
    program = write(tmp_path, b"#!/bin/sh\n")

    with pytest.raises(wheel.WheelError, match="neither a Mach-O, an ELF nor a PE file"):
        wheel.strip_page_program(program, tmp_path)


def test_a_file_that_is_not_a_pdb_is_refused(tmp_path: Path) -> None:
    library = write(tmp_path, pe_library())
    (tmp_path / PDB_NAME).write_bytes(b"not a PDB")

    with pytest.raises(wheel.WheelError, match="is not a PDB"):
        wheel.strip_library(library, tmp_path)


def build_wheel(
    directory: Path, library: Path | None = None, page_program: Path | None = None
) -> subprocess.CompletedProcess[str]:
    """Build this repository's wheel with hatchling, with the library and the Page program in the hook's variables.

    Without a Page program it builds the wheel as the release workflow does.
    """
    named = {wheel.LIBRARY_ENV_VAR: library, wheel.PAGE_PROGRAM_ENV_VAR: page_program}
    env = {name: value for name, value in os.environ.items() if name not in named}
    env |= {name: str(path) for name, path in named.items() if path is not None}
    command = [sys.executable, "-m", "hatchling", "build", "--target", "wheel", "--directory", directory]
    return subprocess.run(command, cwd=REPOSITORY, env=env, capture_output=True, text=True, check=False)


def test_a_wheel_without_a_library_is_refused(tmp_path: Path) -> None:
    result = build_wheel(tmp_path)

    assert result.returncode != 0
    assert f"set {wheel.LIBRARY_ENV_VAR} to the native library" in result.stderr
    assert not list(tmp_path.glob("*.whl"))


def test_a_wheel_from_a_missing_library_is_refused(tmp_path: Path) -> None:
    result = build_wheel(tmp_path, tmp_path / "missing")

    assert result.returncode != 0
    assert f"{wheel.LIBRARY_ENV_VAR} names {tmp_path / 'missing'}, which is not a file" in result.stderr
    assert not list(tmp_path.glob("*.whl"))


def test_a_wheel_from_a_missing_page_program_is_refused(pe_build: Path, tmp_path: Path) -> None:
    result = build_wheel(tmp_path, pe_build / wheel.WINDOWS_LIBRARY, tmp_path / "missing")

    assert result.returncode != 0
    assert f"{wheel.PAGE_PROGRAM_ENV_VAR} names {tmp_path / 'missing'}, which is not a file" in result.stderr
    assert not list(tmp_path.glob("*.whl"))


@pytest.fixture(scope="module")
def pe_build(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Return a made-up build of the DLL and of the Page program beside it, each with its PDB."""
    directory = tmp_path_factory.mktemp("pe-build")
    (directory / wheel.WINDOWS_LIBRARY).write_bytes(pe_library())
    (directory / PDB_NAME).write_bytes(pdb_file())
    (directory / wheel.WINDOWS_PAGE_PROGRAM).write_bytes(pe_page_program())
    (directory / PAGE_PROGRAM_PDB_NAME).write_bytes(pdb_file(PAGE_PROGRAM_PDB_GUID))
    return directory


@pytest.fixture(scope="module")
def dll_wheel(pe_build: Path, tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build a wheel from a made-up DLL, as the release workflow does, which any Host can do.

    hatchling never loads the library, and the Page program beside it stays out, because nothing names it.
    """
    directory = tmp_path_factory.mktemp("dll")

    result = build_wheel(directory, pe_build / wheel.WINDOWS_LIBRARY)
    assert result.returncode == 0, result.stderr
    (built,) = directory.glob("*.whl")
    return built


@pytest.fixture(scope="module")
def page_program_dll_wheel(pe_build: Path, tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build a wheel from a made-up DLL and Page program, as CI does for its tests."""
    directory = tmp_path_factory.mktemp("dll-page-program")

    result = build_wheel(directory, pe_build / wheel.WINDOWS_LIBRARY, pe_build / wheel.WINDOWS_PAGE_PROGRAM)
    assert result.returncode == 0, result.stderr
    (built,) = directory.glob("*.whl")
    return built


def page_programs_in(built: Path) -> list[str]:
    """Return the names under which a wheel holds a Page program."""
    with zipfile.ZipFile(built) as archive:
        return [name for name in archive.namelist() if name.startswith(f"{wheel.PACKAGE}/{wheel.PAGE_PROGRAM}")]


def test_a_wheel_built_as_the_release_workflow_builds_it_holds_no_page_program(dll_wheel: Path) -> None:
    with tarfile.open(wheel.symbols_archive_of(dll_wheel)) as archive:
        symbols = archive.getnames()

    assert page_programs_in(dll_wheel) == []
    assert symbols == [PDB_NAME]


def test_the_release_workflow_names_no_page_program() -> None:
    assert wheel.PAGE_PROGRAM_ENV_VAR not in RELEASE_WORKFLOW.read_text(encoding="utf-8")


def test_a_wheel_holds_the_page_program_it_is_given_and_the_programs_pdb(page_program_dll_wheel: Path) -> None:
    with zipfile.ZipFile(page_program_dll_wheel) as archive:
        library = archive.read(f"mitto/{wheel.WINDOWS_LIBRARY}")
        program = archive.read(f"mitto/{wheel.WINDOWS_PAGE_PROGRAM}")
    with tarfile.open(wheel.symbols_archive_of(page_program_dll_wheel)) as archive:
        symbols = archive.getnames()

    assert page_program_dll_wheel.name.endswith("-py3-none-win_amd64.whl")
    assert page_programs_in(page_program_dll_wheel) == [f"mitto/{wheel.WINDOWS_PAGE_PROGRAM}"]
    assert (library, program) == (pe_library(), pe_page_program())
    assert symbols == [PDB_NAME, PAGE_PROGRAM_PDB_NAME]


def test_a_wheel_of_a_dll_is_for_windows_and_its_symbols_are_the_pdb(dll_wheel: Path, tmp_path: Path) -> None:
    with zipfile.ZipFile(dll_wheel) as archive:
        (metadata,) = (name for name in archive.namelist() if name.endswith(".dist-info/WHEEL"))
        lines = archive.read(metadata).decode().splitlines()
        library = archive.read(f"mitto/{wheel.WINDOWS_LIBRARY}")
    with tarfile.open(wheel.symbols_archive_of(dll_wheel)) as archive:
        names = archive.getnames()
        archive.extractall(tmp_path, filter="data")

    assert dll_wheel.name.endswith("-py3-none-win_amd64.whl")
    assert "Tag: py3-none-win_amd64" in lines
    assert "Root-Is-Purelib: false" in lines
    assert library == pe_library()
    assert names == [PDB_NAME]
    assert (tmp_path / PDB_NAME).read_bytes() == pdb_file()


@pytest.fixture(scope="module")
def stub_build(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Compile a stub of the native core and of the Page program, with their object files kept for debug information."""
    if sys.platform == "win32":
        pytest.skip("a DLL from cc has no PDB, so a made-up DLL stands in for the stub on Windows")
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("no C compiler")
    directory = tmp_path_factory.mktemp("stub")
    (directory / "stub.c").write_text(STUB)
    (directory / "stub_program.c").write_text(STUB_PROGRAM)
    flags: list[str] = [f"-mmacosx-version-min={STUB_MACOS_VERSION}"] if sys.platform == "darwin" else []
    run([compiler, "-c", "-g", "-fPIC", *flags, "-o", directory / "stub.o", directory / "stub.c"])
    run([compiler, "-shared", *flags, "-o", directory / _native.LIBRARY_NAME, directory / "stub.o"])
    run([compiler, "-c", "-g", *flags, "-o", directory / "stub_program.o", directory / "stub_program.c"])
    run([compiler, *flags, "-o", directory / wheel.PAGE_PROGRAM, directory / "stub_program.o"])
    return directory


@pytest.fixture(scope="module")
def stub_wheel(stub_build: Path, tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build a wheel from the stub of the native core."""
    directory = tmp_path_factory.mktemp("stub-wheel")

    result = build_wheel(directory, stub_build / _native.LIBRARY_NAME)
    assert result.returncode == 0, result.stderr
    (built,) = directory.glob("*.whl")
    return built


@pytest.fixture(scope="module")
def page_program_stub_wheel(stub_build: Path, tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build a wheel from the stubs of the native core and of the Page program."""
    directory = tmp_path_factory.mktemp("stub-page-program-wheel")

    result = build_wheel(directory, stub_build / _native.LIBRARY_NAME, stub_build / wheel.PAGE_PROGRAM)
    assert result.returncode == 0, result.stderr
    (built,) = directory.glob("*.whl")
    return built


def unpack(built: Path, directory: Path, binary: str = _native.LIBRARY_NAME) -> tuple[Path, Path]:
    """Unpack a library or program of a wheel and its symbols archive, and return the binary and the file of its symbols."""
    with zipfile.ZipFile(built) as archive:
        unpacked = Path(archive.extract(f"mitto/{binary}", directory))
    with tarfile.open(wheel.symbols_archive_of(built)) as archive:
        archive.extractall(directory, filter="data")
    if sys.platform == "darwin":
        return unpacked, directory / f"{binary}.dSYM/Contents/Resources/DWARF/{binary}"
    if sys.platform == "win32":
        return unpacked, directory / f"{binary}.pdb"
    return unpacked, directory / f"{binary}.debug"


def check_symbols(built: Path, function: str, directory: Path, binary: str = _native.LIBRARY_NAME) -> None:
    """Check that the symbols archive of a wheel belongs to one of its binaries and names a function that it does not."""
    library, symbols = unpack(built, directory, binary)
    if sys.platform == "win32":
        _, name = debug_help_lookup(library, symbols.parent, function)
        assert function not in PeFile(library).exported_names()
        assert name == function
        return
    if sys.platform == "darwin":
        assert uuid_of(library) == uuid_of(symbols)
    else:
        name, checksum = debug_link_of(library, directory)
        assert name == symbols.name
        assert checksum == zlib.crc32(symbols.read_bytes())

    (address,) = (
        address
        for name, address in functions_in(symbols).items()
        if name == function or name.startswith(f"{function}(")
    )
    assert not any(name.startswith(function) for name in functions_in(library))
    assert function in function_at(symbols, address + 1)


def functions_in(file: Path) -> dict[str, int]:
    """Return the functions in the symbol table of a file, by their demangled names."""
    # A stripped ELF file has no symbol table, which nm reports without failing.
    output = subprocess.run(["nm", "-C", file], capture_output=True, text=True, check=False).stdout
    functions: dict[str, int] = {}
    for line in output.splitlines():
        match line.split(maxsplit=2):
            case [address, "t" | "T", name]:
                # Mach-O puts an underscore before each C name.
                functions[name.removeprefix("_") if sys.platform == "darwin" else name] = int(address, 16)
            case _:
                pass
    return functions


def function_at(symbols: Path, address: int) -> str:
    """Return what the debugger's tool of this Host makes of an address in the library, with the library's symbols."""
    if sys.platform == "darwin":
        return run(["atos", "-o", symbols, "-l", "0", hex(address)])
    return run(["addr2line", "--functions", "--demangle", "--exe", symbols, hex(address)])


_SYMBOL_NAME_SIZE = 1024


class _SymbolInfo(ctypes.Structure):
    """SYMBOL_INFOW of Windows' debug help library, with room for a long name."""

    _fields_ = (
        ("SizeOfStruct", ctypes.c_uint32),
        ("TypeIndex", ctypes.c_uint32),
        ("Reserved", ctypes.c_uint64 * 2),
        ("Index", ctypes.c_uint32),
        ("Size", ctypes.c_uint32),
        ("ModBase", ctypes.c_uint64),
        ("Flags", ctypes.c_uint32),
        ("Value", ctypes.c_uint64),
        ("Address", ctypes.c_uint64),
        ("Register", ctypes.c_uint32),
        ("Scope", ctypes.c_uint32),
        ("Tag", ctypes.c_uint32),
        ("NameLen", ctypes.c_uint32),
        ("MaxNameLen", ctypes.c_uint32),
        ("Name", ctypes.c_wchar * _SYMBOL_NAME_SIZE),
    )


# sizeof(SYMBOL_INFOW), whose name has room for one character.
_SYMBOL_INFO_SIZE = 88
_SYMOPT_UNDNAME = 0x2
_SYMOPT_FAIL_CRITICAL_ERRORS = 0x200
_SYMOPT_EXACT_SYMBOLS = 0x400
# The debug help library names a session by a process handle, or by any other value when it reads no process.
_DEBUG_HELP_SESSION = 0x6D6974746F


def debug_help_lookup(library: Path, symbol_path: Path, function: str) -> tuple[int, str]:
    """Return the address of a function in a DLL, and the function that the address one past it lies in.

    Both come from Windows' debug help library, the one that Windows' debuggers read symbols with. It looks for the PDB
    that the DLL names in the symbol path, and with exact symbols it takes it only if its GUID and age are the DLL's.
    """
    if sys.platform != "win32":
        raise NotImplementedError("the debug help library is Windows'")
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    dbghelp.SymInitializeW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int)
    dbghelp.SymLoadModuleExW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.c_wchar_p,
        ctypes.c_uint64,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_uint32,
    )
    dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
    dbghelp.SymFromNameW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(_SymbolInfo))
    dbghelp.SymFromAddrW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.POINTER(_SymbolInfo),
    )
    dbghelp.SymCleanup.argtypes = (ctypes.c_void_p,)
    dbghelp.SymSetOptions(_SYMOPT_UNDNAME | _SYMOPT_FAIL_CRITICAL_ERRORS | _SYMOPT_EXACT_SYMBOLS)
    if not dbghelp.SymInitializeW(_DEBUG_HELP_SESSION, str(symbol_path), False):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not dbghelp.SymLoadModuleExW(_DEBUG_HELP_SESSION, None, str(library), None, 0, 0, None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        info = _SymbolInfo(SizeOfStruct=_SYMBOL_INFO_SIZE, MaxNameLen=_SYMBOL_NAME_SIZE)
        if not dbghelp.SymFromNameW(_DEBUG_HELP_SESSION, function, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        address: int = info.Address
        info = _SymbolInfo(SizeOfStruct=_SYMBOL_INFO_SIZE, MaxNameLen=_SYMBOL_NAME_SIZE)
        displacement = ctypes.c_uint64()
        if not dbghelp.SymFromAddrW(_DEBUG_HELP_SESSION, address + 1, ctypes.byref(displacement), ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        name: str = info.Name
        return address, name
    finally:
        dbghelp.SymCleanup(_DEBUG_HELP_SESSION)


def uuid_of(file: Path) -> str:
    match = re.search(r"UUID: (\S+)", run(["dwarfdump", "--uuid", file]))
    assert match is not None
    return match[1]


def debug_link_of(library: Path, directory: Path) -> tuple[str, int]:
    """Return the file name and the checksum in a library's link to its symbols."""
    section = directory / "debug_link"
    run(["objcopy", f"--dump-section=.gnu_debuglink={section}", library, directory / "discarded"])
    data = section.read_bytes()
    return data[: data.index(b"\0")].decode(), int.from_bytes(data[-4:], "little")


def test_the_wheel_holds_the_library_next_to_the_loader(stub_wheel: Path) -> None:
    with zipfile.ZipFile(stub_wheel) as archive:
        names = archive.namelist()
        (metadata,) = (name for name in names if name.endswith(".dist-info/WHEEL"))
        lines = archive.read(metadata).decode().splitlines()

    tag = stub_wheel.name.removesuffix(".whl").split("-", 2)[2]
    if sys.platform == "darwin":
        assert tag == f"py3-none-macosx_{STUB_MACOS_VERSION.replace('.', '_')}_arm64"
    else:
        assert re.fullmatch(r"py3-none-manylinux_2_\d+_x86_64", tag)
    assert {"mitto/_native.py", f"mitto/{_native.LIBRARY_NAME}"} <= set(names)
    assert "Root-Is-Purelib: false" in lines
    assert f"Tag: {tag}" in lines


def licence_files_of(built: Path) -> dict[str, bytes]:
    """Return the licence files in a wheel's .dist-info directory, by name, and check that its metadata names each."""
    with zipfile.ZipFile(built) as archive:
        names = archive.namelist()
        (metadata,) = (name for name in names if name.endswith(".dist-info/METADATA"))
        directory = f"{metadata.removesuffix('METADATA')}licenses/"
        files = {name.removeprefix(directory): archive.read(name) for name in names if name.startswith(directory)}
        lines = archive.read(metadata).decode().splitlines()
    assert [line.removeprefix("License-File: ") for line in lines if line.startswith("License-File: ")] == sorted(files)
    return files


def test_the_wheel_holds_the_licence_and_the_notices(dll_wheel: Path) -> None:
    assert licence_files_of(dll_wheel) == {
        "LICENSE": (REPOSITORY / "LICENSE").read_bytes(),
        "MACHINE_TABLE_NOTICE": (REPOSITORY / "MACHINE_TABLE_NOTICE").read_bytes(),
        notices.NOTICES_FILE.name: notices.NOTICES_FILE.read_bytes(),
    }


def test_the_wheel_declares_both_licences_and_holds_the_table(dll_wheel: Path) -> None:
    with zipfile.ZipFile(dll_wheel) as archive:
        (metadata,) = (name for name in archive.namelist() if name.endswith(".dist-info/METADATA"))
        lines = archive.read(metadata).decode().splitlines()
        table = archive.read("mitto/machine_table.json")

    assert "License-Expression: Apache-2.0 AND CC-BY-SA-3.0" in lines
    assert table == (REPOSITORY / "src" / "mitto" / "machine_table.json").read_bytes()


def test_the_stripped_stub_loads(stub_wheel: Path, tmp_path: Path) -> None:
    library, _ = unpack(stub_wheel, tmp_path)

    assert ctypes.CDLL(library).mitto_abi_version() in {3, 4}


def test_the_symbols_of_the_stub_name_a_function_the_library_does_not(stub_wheel: Path, tmp_path: Path) -> None:
    check_symbols(stub_wheel, STUB_HIDDEN_FUNCTION, tmp_path)


def test_the_stub_wheel_holds_no_page_program(stub_wheel: Path) -> None:
    assert page_programs_in(stub_wheel) == []


def test_the_stripped_page_program_is_executable_and_runs(page_program_stub_wheel: Path, tmp_path: Path) -> None:
    with zipfile.ZipFile(page_program_stub_wheel) as archive:
        mode = archive.getinfo(f"mitto/{wheel.PAGE_PROGRAM}").external_attr >> 16
    program, _ = unpack(page_program_stub_wheel, tmp_path, wheel.PAGE_PROGRAM)
    # zipfile does not set the mode it unpacks, as an installer does.
    program.chmod(stat.S_IMODE(mode))

    assert stat.S_IMODE(mode) == 0o755
    assert run([program]) == "42\n"


def test_the_symbols_of_the_page_program_name_a_function_it_does_not(
    page_program_stub_wheel: Path, tmp_path: Path
) -> None:
    check_symbols(page_program_stub_wheel, STUB_PROGRAM_HIDDEN_FUNCTION, tmp_path, wheel.PAGE_PROGRAM)
    check_symbols(page_program_stub_wheel, STUB_HIDDEN_FUNCTION, tmp_path / "library")


@pytest.fixture(scope="module")
def built_wheel() -> Path:
    """Return the wheel of the real native core that MITTO_WHEEL names, or skip if there is none."""
    value = os.environ.get(WHEEL_ENV_VAR)
    if not value:
        pytest.skip(f"no wheel of the native core; set {WHEEL_ENV_VAR} to run this test")
    return Path(value).resolve()


def test_the_installed_package_holds_the_library_of_the_wheel(built_wheel: Path) -> None:
    package = Path(_native.__file__).parent

    assert _native.LIBRARY_ENV_VAR not in os.environ
    assert package != REPOSITORY / "src" / "mitto"
    assert _native.find_library() == package / _native.LIBRARY_NAME
    with zipfile.ZipFile(built_wheel) as archive:
        assert archive.read(f"mitto/{_native.LIBRARY_NAME}") == (package / _native.LIBRARY_NAME).read_bytes()
    assert _native.load_library().mitto_abi_version() == _native.ABI_VERSION


def test_the_wheel_is_tagged_for_what_its_library_needs(built_wheel: Path, tmp_path: Path) -> None:
    platform = built_wheel.name.removesuffix(".whl").rsplit("-", 1)[1]
    if sys.platform == "win32":
        library, _ = unpack(built_wheel, tmp_path)
        assert (PeFile(library).machine, platform) == (IMAGE_FILE_MACHINE_AMD64, "win_amd64")
    elif sys.platform == "darwin":
        library, _ = unpack(built_wheel, tmp_path)
        match = re.search(r"minos (\d+)\.(\d+)", run(["otool", "-l", library]))
        assert match is not None
        assert platform == f"macosx_{match[1]}_{match[2]}_arm64"
    else:
        report = json.loads(run([sys.executable, "-m", "auditwheel", "show", "--json", built_wheel]))
        assert report["overall_tag"] == platform


def test_the_symbols_of_the_wheel_name_a_function_the_library_does_not(built_wheel: Path, tmp_path: Path) -> None:
    check_symbols(built_wheel, HIDDEN_FUNCTION, tmp_path)


def test_the_wheel_holds_a_page_program_only_if_the_tests_run_the_installed_one(built_wheel: Path) -> None:
    # CI names the Page program installed from its wheel. The release workflow names none, so its wheels hold none.
    named = os.environ.get(PAGE_PROGRAM_ENV_VAR)
    package = Path(_native.__file__).parent
    installed = [package / Path(name).name for name in page_programs_in(built_wheel)]

    if named and Path(named).resolve().parent == package.resolve():
        assert [path.resolve() for path in installed] == [Path(named).resolve()]
    else:
        assert installed == []


def test_the_wheel_holds_the_notice_of_every_project_its_library_links(built_wheel: Path) -> None:
    value = os.environ.get(HOST_NOTICES_ENV_VAR)
    if not value:
        pytest.skip(f"nothing says what the library links; set {HOST_NOTICES_ENV_VAR} to run this test")
    linked = notices.parse(Path(value).read_text(encoding="utf-8"))
    host = export.current_host().name
    in_wheel = notices.parse(licence_files_of(built_wheel)[notices.NOTICES_FILE.name].decode())

    assert notices.host_of(linked) == host
    assert notices.differences(in_wheel, host, linked) == []
