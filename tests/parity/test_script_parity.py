"""The Parity Checks of a Script Fingerprint, through a real Page of the native core at the capture server's case page.

Tests that take the page_program fixture need the native library with the Page program beside it, as CI's native job
installs it on every Host, and are skipped elsewhere.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto._native import LIBRARY_ENV_VAR
from mitto._persona import WINDOWS_PERSONAS
from tools.parity import BASELINES_DIRECTORY, dumps_known_gaps, mismatches, script
from tools.parity.script import find_script_gaps, load_script_baselines
from tools.parity.script_gaps import SCRIPT_KNOWN_GAPS_FILE_NAME, GapKind, loads_script_known_gaps

_REPOSITORY = Path(__file__).parents[2]
_REGENERATE = "uv run python -m tools.parity --script"


def listed(directory: Path) -> list[Any]:
    return loads_script_known_gaps((directory / SCRIPT_KNOWN_GAPS_FILE_NAME).read_text(encoding="utf-8"))


def regenerate(directory: Path, **environment: str) -> subprocess.CompletedProcess[str]:
    """Run the command that writes the known-gaps list of a Script Fingerprint again."""
    return subprocess.run(
        [sys.executable, "-m", "tools.parity", "--script", "--baselines", str(directory)],
        cwd=_REPOSITORY,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.fixture
def baselines_of_157(tmp_path: Path) -> Path:
    """Return a directory with the Baseline of Chrome 157's page, and no known-gaps list."""
    (tmp_path / "157" / "script").mkdir(parents=True)
    shutil.copy(BASELINES_DIRECTORY / "157" / "script" / "page.json", tmp_path / "157" / "script" / "page.json")
    return tmp_path


@pytest.mark.usefixtures("page_program", "both_apis")
def test_a_pages_script_fingerprint_differs_from_the_baselines_only_by_the_known_gaps(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """What M7 is measured with, through each API, on every Host with one list."""
    monkeypatch.setattr(script, "Session", mitto.Session)
    found = mismatches(find_script_gaps(load_script_baselines(BASELINES_DIRECTORY)), listed(BASELINES_DIRECTORY))

    if found:
        pytest.fail(f"{found}\n\nIf this is what you meant, write the list again: {_REGENERATE}", pytrace=False)


def test_the_list_is_written_the_way_the_command_writes_it() -> None:
    text = (BASELINES_DIRECTORY / SCRIPT_KNOWN_GAPS_FILE_NAME).read_text(encoding="utf-8")

    assert dumps_known_gaps(loads_script_known_gaps(text)) == text


def test_each_gap_in_the_list_is_of_a_release_with_a_persona_and_has_a_kind() -> None:
    gaps = listed(BASELINES_DIRECTORY)

    assert {gap.release for gap in gaps} <= set(WINDOWS_PERSONAS)
    assert {gap.kind for gap in gaps} <= set(GapKind)


@pytest.mark.usefixtures("page_program")
def test_a_written_list_makes_the_run_pass_and_says_how_many_of_each_kind(baselines_of_157: Path) -> None:
    result = regenerate(baselines_of_157)

    assert result.returncode == 0, result.stderr
    gaps = listed(baselines_of_157)
    assert f"Wrote {len(gaps)} gaps" in result.stdout
    for kind in GapKind:
        assert f"  {kind}: {sum(gap.kind == kind for gap in gaps)}\n" in result.stdout
    assert f"  157: {len(gaps)}\n" in result.stdout
    again = regenerate(baselines_of_157)
    assert ": 0 new, 0 gone." in again.stdout


@pytest.mark.usefixtures("page_program")
def test_a_deliberate_change_to_a_baseline_fails_the_run(baselines_of_157: Path) -> None:
    gaps = find_script_gaps(load_script_baselines(baselines_of_157))
    path = baselines_of_157 / "157" / "script" / "page.json"
    form = json.loads(path.read_text(encoding="utf-8"))
    navigator = next(entry for entry in form["objects"] if entry["path"] == "Navigator.prototype")
    navigator["own"] = [own for own in navigator["own"] if own[0] != "vendor"]
    path.write_text(json.dumps(form), encoding="utf-8")

    found = mismatches(find_script_gaps(load_script_baselines(baselines_of_157)), gaps)

    assert [(gap.kind, gap.field, gap.baseline) for gap in found.unlisted] == [
        (GapKind.SHAPE, "Navigator.prototype own vendor", None)
    ]
    assert found.gone == []


def test_the_command_needs_a_native_library_and_leaves_the_list_alone(baselines_of_157: Path) -> None:
    known = baselines_of_157 / SCRIPT_KNOWN_GAPS_FILE_NAME
    known.write_text("[]\n")

    result = regenerate(baselines_of_157, **{LIBRARY_ENV_VAR: str(baselines_of_157 / "no_such_library")})

    assert result.returncode == 1
    assert LIBRARY_ENV_VAR in result.stderr
    assert known.read_text() == "[]\n"


def test_the_command_refuses_a_chrome_release_with_no_persona_and_leaves_the_list_alone(
    baselines_of_157: Path,
) -> None:
    known = baselines_of_157 / SCRIPT_KNOWN_GAPS_FILE_NAME
    known.write_text("[]\n")
    (baselines_of_157 / "157").rename(baselines_of_157 / "154")

    result = regenerate(baselines_of_157)

    assert result.returncode == 1
    assert "Chrome 154" in result.stderr
    assert known.read_text() == "[]\n"


def test_the_command_refuses_a_bad_list_and_leaves_it_alone(baselines_of_157: Path) -> None:
    known = baselines_of_157 / SCRIPT_KNOWN_GAPS_FILE_NAME
    known.write_text("not a list")

    result = regenerate(baselines_of_157)

    assert result.returncode == 1
    assert "known-gaps" in result.stderr
    assert known.read_text() == "not a list"
