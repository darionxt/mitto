"""Tests of the version of the messages between a Page and its Session, which the build works out.

native/BUILD.gn's page_protocol_version action runs native/page_protocol_version.py on the files that define the bytes
of a message, and the tests run that script as GN does, with no build.
"""

import hashlib
import re
import shutil
from pathlib import Path

import pytest

from tests.page_protocol import NATIVE, hashed_files, protocol_version

# The headers of the library's that a hashed file includes but that define no bytes of a message: the Fetch Standard's
# checks of a header line, and the version itself.
UNHASHED_INCLUDES = frozenset({"header_rules.h", "page_protocol_version.h"})
HASHED_FILES = hashed_files()


def copy_of_hashed_files(tmp_path: Path) -> Path:
    copy = tmp_path / "native"
    copy.mkdir()
    for name in HASHED_FILES:
        (copy / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(NATIVE / name, copy / name)
    return copy


def test_the_version_is_the_start_of_the_sha256_of_each_files_name_and_text(tmp_path: Path) -> None:
    digest = hashlib.sha256()
    for name in sorted(HASHED_FILES):
        digest.update(name.encode() + b"\0" + (NATIVE / name).read_bytes().replace(b"\r\n", b"\n") + b"\0")

    assert protocol_version(NATIVE, HASHED_FILES, tmp_path) == int(digest.hexdigest()[:8], 16)


@pytest.mark.parametrize("name", HASHED_FILES)
def test_a_change_to_any_hashed_file_changes_the_version(tmp_path: Path, name: str) -> None:
    copy = copy_of_hashed_files(tmp_path)
    before = protocol_version(copy, HASHED_FILES, tmp_path)

    with (copy / name).open("ab") as changed:
        changed.write(b"\n")

    assert protocol_version(copy, HASHED_FILES, tmp_path) != before


def test_line_endings_and_the_order_of_the_names_leave_the_version_as_it_is(tmp_path: Path) -> None:
    copy = copy_of_hashed_files(tmp_path)
    for name in HASHED_FILES:
        path = copy / name
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))

    assert protocol_version(copy, HASHED_FILES[::-1], tmp_path) == protocol_version(NATIVE, HASHED_FILES, tmp_path)


def test_every_file_that_writes_or_reads_a_payload_is_hashed() -> None:
    # The files of each family of messages, beside page_messages.h, and whatever else writes or reads one with
    # message_coding.h. Those of the bindings' exception_messages.h are what a page's scripts read. Sources are bytes
    # here, since a few hold UTF-8 that the locale's encoding may not read.
    sources = {
        path.relative_to(NATIVE): path.read_bytes()
        for path in NATIVE.rglob("*")
        if path.suffix in {".h", ".cc"} and "test" not in path.relative_to(NATIVE).parts
    }
    messages = {
        name.as_posix()
        for name, text in sources.items()
        if (len(name.parts) == 1 and name.stem.endswith("_messages")) or b'#include "mitto/message_coding.h"' in text
    }

    assert messages - set(HASHED_FILES) == set()


def test_both_ends_ways_of_writing_a_storage_key_are_hashed() -> None:
    # The library writes its own into each broadcast for a Page, which matches it against its contexts' own.
    assert {"dom/storage_key.cc", "dom/storage_key.h", "storage/storage_key.cc", "storage/storage_key.h"} <= set(
        hashed_files("_page_message_counterparts")
    )


def test_every_header_of_the_library_that_a_hashed_file_includes_is_hashed() -> None:
    included = {
        header
        for name in HASHED_FILES
        for header in re.findall(
            r'^#include "mitto/([^"]+)"$', (NATIVE / name).read_text(encoding="utf-8"), re.MULTILINE
        )
    }

    assert included - set(HASHED_FILES) - UNHASHED_INCLUDES == set()
