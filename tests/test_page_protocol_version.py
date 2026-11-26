"""Tests of the version of the messages between a Page and its Session, which the build works out.

native/BUILD.gn's page_protocol_version action runs native/page_protocol_version.py on the files that define the bytes
of a message, and the tests run that script as GN does, with no build.
"""

import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

NATIVE = Path(__file__).parent.parent / "native"
SCRIPT = NATIVE / "page_protocol_version.py"
# The headers of the library's that a hashed file includes but that define no bytes of a message: the Fetch Standard's
# checks of a header line, and the version itself.
UNHASHED_INCLUDES = frozenset({"header_rules.h", "page_protocol_version.h"})


def hashed_files() -> list[str]:
    """Return the files that the action hashes, as BUILD.gn names them, relative to native/."""
    build = (NATIVE / "BUILD.gn").read_text()
    listed = re.search(r"^_page_message_sources = \[(.*?)^\]", build, re.MULTILINE | re.DOTALL)
    assert listed, "BUILD.gn has no _page_message_sources"
    return re.findall(r'"([^"]+)"', listed[1])


HASHED_FILES = hashed_files()


def version(source_dir: Path, names: list[str], tmp_path: Path) -> int:
    """Return the version that the script writes for the files of `names` in `source_dir`."""
    header = tmp_path / "page_protocol_version.h"
    subprocess.run([sys.executable, SCRIPT, source_dir, header, *names], check=True)
    written = header.read_text()
    found = re.search(r"^inline constexpr uint32_t kPageProtocolVersion = 0x([0-9a-f]{8});$", written, re.MULTILINE)
    assert found, written
    return int(found[1], 16)


def copy_of_hashed_files(tmp_path: Path) -> Path:
    copy = tmp_path / "native"
    copy.mkdir()
    for name in HASHED_FILES:
        shutil.copyfile(NATIVE / name, copy / name)
    return copy


def test_the_version_is_the_start_of_the_sha256_of_each_files_name_and_text(tmp_path: Path) -> None:
    digest = hashlib.sha256()
    for name in sorted(HASHED_FILES):
        digest.update(name.encode() + b"\0" + (NATIVE / name).read_bytes().replace(b"\r\n", b"\n") + b"\0")

    assert version(NATIVE, HASHED_FILES, tmp_path) == int(digest.hexdigest()[:8], 16)


@pytest.mark.parametrize("name", HASHED_FILES)
def test_a_change_to_any_hashed_file_changes_the_version(tmp_path: Path, name: str) -> None:
    copy = copy_of_hashed_files(tmp_path)
    before = version(copy, HASHED_FILES, tmp_path)

    with (copy / name).open("a") as changed:
        changed.write("\n")

    assert version(copy, HASHED_FILES, tmp_path) != before


def test_line_endings_and_the_order_of_the_names_leave_the_version_as_it_is(tmp_path: Path) -> None:
    copy = copy_of_hashed_files(tmp_path)
    for name in HASHED_FILES:
        path = copy / name
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))

    assert version(copy, HASHED_FILES[::-1], tmp_path) == version(NATIVE, HASHED_FILES, tmp_path)


def test_every_file_that_writes_or_reads_a_payload_is_hashed() -> None:
    # The files of each family of messages, beside page_messages.h, and whatever else writes or reads one with
    # message_coding.h. Those of the bindings' exception_messages.h are what a page's scripts read.
    sources = [path for path in NATIVE.rglob("*") if path.suffix in {".h", ".cc"} and "test" not in path.parts]
    messages = {
        path.relative_to(NATIVE).as_posix()
        for path in sources
        if (path.parent == NATIVE and path.stem.endswith("_messages"))
        or '#include "mitto/message_coding.h"' in path.read_text()
    }

    assert messages - set(HASHED_FILES) == set()


def test_every_header_of_the_library_that_a_hashed_file_includes_is_hashed() -> None:
    included = {
        header
        for name in HASHED_FILES
        for header in re.findall(r'^#include "mitto/([^"]+)"$', (NATIVE / name).read_text(), re.MULTILINE)
    }

    assert included - set(HASHED_FILES) - UNHASHED_INCLUDES == set()
