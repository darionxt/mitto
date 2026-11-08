"""Tests of the build hook that puts the native core into the wheel.

Tests of the tag read made-up headers. Tests of the build run hatchling on this repository. Those that need a library
compile a stub with cc, and are skipped when there is no C compiler. Those that take the built_wheel fixture check a wheel
of the real native core, named in MITTO_WHEEL, and the package installed from it. The test of its notices also needs
what the library links, which `python -m tools.workspace.notices host` prints from the library's build, in
MITTO_HOST_NOTICES.
"""

import ctypes
import json
import os
import re
import shutil
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
from tools import wheel
from tools.workspace import export, notices

REPOSITORY = Path(__file__).parents[1]

# Names a wheel of the real native core, with its symbols archive next to it.
WHEEL_ENV_VAR = "MITTO_WHEEL"

# Names this Host's part of the notices, from the build of the wheel's library.
HOST_NOTICES_ENV_VAR = "MITTO_HOST_NOTICES"

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

CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007
PLATFORM_IOS = 2
EM_AARCH64 = 183
SHT_STRTAB = 3


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


def test_a_file_that_is_not_a_library_has_no_tag(tmp_path: Path) -> None:
    library = write(tmp_path, b"#!/bin/sh\n")

    with pytest.raises(wheel.WheelError, match="neither a Mach-O nor an ELF file"):
        wheel.platform_tag(library)


def build_wheel(directory: Path, library: Path | None = None) -> subprocess.CompletedProcess[str]:
    """Build this repository's wheel with hatchling, and the library in the hook's variable if one is given."""
    env = {name: value for name, value in os.environ.items() if name != wheel.LIBRARY_ENV_VAR}
    if library is not None:
        env[wheel.LIBRARY_ENV_VAR] = str(library)
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


@pytest.fixture(scope="module")
def stub_wheel(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build a wheel from a stub of the native core, with its object file kept for the debug information."""
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("no C compiler")
    directory = tmp_path_factory.mktemp("stub")
    source = directory / "stub.c"
    source.write_text(STUB)
    flags: list[str] = [f"-mmacosx-version-min={STUB_MACOS_VERSION}"] if sys.platform == "darwin" else []
    run([compiler, "-c", "-g", "-fPIC", *flags, "-o", directory / "stub.o", source])
    run([compiler, "-shared", *flags, "-o", directory / _native.LIBRARY_NAME, directory / "stub.o"])

    result = build_wheel(directory / "dist", directory / _native.LIBRARY_NAME)
    assert result.returncode == 0, result.stderr
    (built,) = (directory / "dist").glob("*.whl")
    return built


def unpack(built: Path, directory: Path) -> tuple[Path, Path]:
    """Unpack the library of a wheel and its symbols archive, and return the library and the file of its symbols."""
    with zipfile.ZipFile(built) as archive:
        library = Path(archive.extract(f"mitto/{_native.LIBRARY_NAME}", directory))
    with tarfile.open(wheel.symbols_archive_of(built)) as archive:
        archive.extractall(directory, filter="data")
    if sys.platform == "darwin":
        return library, directory / f"{_native.LIBRARY_NAME}.dSYM/Contents/Resources/DWARF/{_native.LIBRARY_NAME}"
    return library, directory / f"{_native.LIBRARY_NAME}.debug"


def check_symbols(built: Path, function: str, directory: Path) -> None:
    """Check that the symbols archive of a wheel belongs to its library and names a function that the library does not."""
    library, symbols = unpack(built, directory)
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


def test_the_wheel_holds_the_licence_and_the_notices(stub_wheel: Path) -> None:
    assert licence_files_of(stub_wheel) == {
        "LICENSE": (REPOSITORY / "LICENSE").read_bytes(),
        "MACHINE_TABLE_NOTICE": (REPOSITORY / "MACHINE_TABLE_NOTICE").read_bytes(),
        notices.NOTICES_FILE.name: notices.NOTICES_FILE.read_bytes(),
    }


def test_the_wheel_declares_both_licences_and_holds_the_table(stub_wheel: Path) -> None:
    with zipfile.ZipFile(stub_wheel) as archive:
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
    if sys.platform == "darwin":
        library, _ = unpack(built_wheel, tmp_path)
        match = re.search(r"minos (\d+)\.(\d+)", run(["otool", "-l", library]))
        assert match is not None
        assert platform == f"macosx_{match[1]}_{match[2]}_arm64"
    else:
        report = json.loads(run([sys.executable, "-m", "auditwheel", "show", "--json", built_wheel]))
        assert report["overall_tag"] == platform


def test_the_symbols_of_the_wheel_name_a_function_the_library_does_not(built_wheel: Path, tmp_path: Path) -> None:
    check_symbols(built_wheel, HIDDEN_FUNCTION, tmp_path)


def test_the_wheel_holds_the_notice_of_every_project_its_library_links(built_wheel: Path) -> None:
    value = os.environ.get(HOST_NOTICES_ENV_VAR)
    if not value:
        pytest.skip(f"nothing says what the library links; set {HOST_NOTICES_ENV_VAR} to run this test")
    linked = notices.parse(Path(value).read_text())
    host = export.current_host().name
    in_wheel = notices.parse(licence_files_of(built_wheel)[notices.NOTICES_FILE.name].decode())

    assert notices.host_of(linked) == host
    assert notices.differences(in_wheel, host, linked) == []
