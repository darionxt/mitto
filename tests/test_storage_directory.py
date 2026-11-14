"""Tests of the file that names a storage directory's Machine, which need no native library."""

import dataclasses
import json
import sys
from pathlib import Path

import pytest

import mitto
from mitto._storage_directory import (
    MACHINE_FILE,
    MACHINE_FILE_VERSION,
    read_storage_machine,
    storage_directory,
    write_storage_machine,
)


def test_a_new_directory_names_no_machine(tmp_path: Path) -> None:
    assert read_storage_machine(tmp_path) is None
    assert read_storage_machine(tmp_path / "not yet made") is None


def test_a_written_machine_reads_back_with_its_seed(tmp_path: Path) -> None:
    machine = mitto.Machine.draw(seed=3)
    write_storage_machine(tmp_path, machine)

    assert read_storage_machine(tmp_path) == machine
    held = json.loads((tmp_path / MACHINE_FILE).read_text())
    assert held == {"version": MACHINE_FILE_VERSION, "machine": dataclasses.asdict(machine)}
    # Only the file is left, with no temporary one beside it.
    assert [path.name for path in tmp_path.iterdir()] == [MACHINE_FILE]


@pytest.mark.skipif(sys.platform == "win32", reason="Windows keeps no POSIX modes")
def test_only_its_owner_can_read_the_file(tmp_path: Path) -> None:
    write_storage_machine(tmp_path, mitto.Machine.draw(seed=3))

    assert (tmp_path / MACHINE_FILE).stat().st_mode & 0o777 == 0o600


@pytest.mark.parametrize(
    "contents",
    [
        pytest.param("not json", id="not-json"),
        pytest.param("[]", id="not-an-object"),
        pytest.param('{"version": 1}', id="no-machine"),
        pytest.param('{"version": 2, "machine": {}}', id="another-version"),
        pytest.param('{"version": true, "machine": {}}', id="a-boolean-version"),
        pytest.param('{"version": 1, "machine": [], "more": 1}', id="another-key"),
        pytest.param('{"version": 1, "machine": []}', id="machine-not-an-object"),
        pytest.param('{"version": 1, "machine": {"memory": 8}}', id="not-a-machine"),
        pytest.param("[" * 100_000 + "]" * 100_000, id="too-deep"),
    ],
)
def test_a_file_that_names_no_machine_is_refused(tmp_path: Path, contents: str) -> None:
    (tmp_path / MACHINE_FILE).write_text(contents)

    with pytest.raises(ValueError, match="does not name a Machine that this Session can claim"):
        read_storage_machine(tmp_path)


def test_the_directory_is_absolute_and_never_empty(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    assert storage_directory(None) is None
    with pytest.raises(ValueError, match="names no directory"):
        storage_directory("")
    monkeypatch.chdir(tmp_path)
    assert storage_directory("kept") == tmp_path.resolve() / "kept"
    assert storage_directory(Path("kept")) == tmp_path.resolve() / "kept"
