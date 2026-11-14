"""The Parity Checks, through the real native core to the capture server, with no external network.

Tests that take the native_library fixture are skipped when this machine has no native library.
"""

import itertools
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import Session
from mitto._native import LIBRARY_ENV_VAR, PrivateSettings
from mitto._persona import CHROME_157_WINDOWS, WINDOWS_PERSONAS, load_persona
from tests.parity.conftest import SAMPLE_RELEASE, ChangeBaseline
from tools.capture import (
    DOCUMENT_RUNS,
    HTTP1,
    HTTP2,
    HTTP3,
    KIND_PATHS,
    RUNS,
    CaptureServer,
    RequestKind,
    Run,
    dumps_normal_form,
    normal_form,
)
from tools.parity import (
    BASELINES_DIRECTORY,
    KNOWN_GAPS_FILE_NAME,
    Baseline,
    Gap,
    check,
    dumps_known_gaps,
    find_gaps,
    load_baselines,
    loads_known_gaps,
    mismatches,
)
from tools.parity.check import record
from tools.parity.replay import protocol_of

_REPOSITORY = Path(__file__).parents[2]
_REGENERATE = "uv run python -m tools.parity"
_ALL_KINDS = ("navigation", "fetch", "script", "image")


def known_gaps(directory: Path) -> list[Gap]:
    return loads_known_gaps((directory / KNOWN_GAPS_FILE_NAME).read_text())


def regenerate(directory: Path, **environment: str) -> subprocess.CompletedProcess[str]:
    """Run the command that regenerates the known-gaps list."""
    return subprocess.run(
        [sys.executable, "-m", "tools.parity", "--baselines", str(directory)],
        cwd=_REPOSITORY,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.usefixtures("native_library", "both_apis")
def test_the_baselines_differ_from_the_native_core_only_by_the_known_gaps(monkeypatch: pytest.MonkeyPatch) -> None:
    """The check that everything in this milestone is measured with, through each API."""
    monkeypatch.setattr(check, "Session", mitto.Session)
    found = mismatches(find_gaps(load_baselines(BASELINES_DIRECTORY)), known_gaps(BASELINES_DIRECTORY))

    if found:
        pytest.fail(f"{found}\n\nIf this is what you meant, regenerate the list: {_REGENERATE}", pytrace=False)


def test_the_known_gaps_list_is_written_the_way_the_command_writes_it() -> None:
    text = (BASELINES_DIRECTORY / KNOWN_GAPS_FILE_NAME).read_text()

    assert dumps_known_gaps(loads_known_gaps(text)) == text


def test_each_release_has_a_baseline_of_every_recording_case_of_every_run() -> None:
    found = {(baseline.release, baseline.case, baseline.protocol) for baseline in load_baselines(BASELINES_DIRECTORY)}

    assert found == {
        (release, case.name, protocol_of(run.route))
        for release, run in itertools.product(WINDOWS_PERSONAS, (*RUNS, *DOCUMENT_RUNS))
        for case in run.cases
    }


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
@pytest.mark.parametrize("run", [*RUNS, *DOCUMENT_RUNS], ids=lambda run: run.name)
def test_each_release_has_every_recording_case_of_every_run(release: str, run: Run) -> None:
    text = (BASELINES_DIRECTORY / release / f"{run.name}.json").read_text(encoding="utf-8")

    form = json.loads(text)

    assert list(form["requests"]) == [case.name for case in run.cases]
    assert dumps_normal_form(form) == text


def test_each_release_says_which_chrome_recorded_it() -> None:
    releases = {baseline.release for baseline in load_baselines(BASELINES_DIRECTORY)}

    for release in releases:
        note = (BASELINES_DIRECTORY / release / "chrome.txt").read_text()
        assert f"Chrome: {release}." in note


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_a_check_presents_the_chrome_release_of_its_baseline(release: str) -> None:
    request = record(Baseline(release, "navigation", HTTP2, {}))["request"]

    assert isinstance(request, dict)
    headers = request["headers"]
    assert isinstance(headers, list)
    assert any(f"Chrome/{release}.0.0.0" in str(line) for line in headers)


@pytest.mark.usefixtures("native_library")
def test_a_regenerated_list_makes_the_run_pass(sample_baselines: Path) -> None:
    result = regenerate(sample_baselines)

    assert result.returncode == 0, result.stderr
    listed = known_gaps(sample_baselines)
    assert f"Wrote {len(listed)} gaps" in result.stdout
    assert f"  navigation: {sum(gap.case == 'navigation' for gap in listed)}\n" in result.stdout
    assert not mismatches(find_gaps(load_baselines(sample_baselines)), listed)


@pytest.mark.usefixtures("native_library")
def test_regenerating_twice_writes_the_same_text(sample_baselines: Path) -> None:
    result = regenerate(sample_baselines)
    assert result.returncode == 0, result.stderr
    first = (sample_baselines / KNOWN_GAPS_FILE_NAME).read_text()

    result = regenerate(sample_baselines)

    assert result.returncode == 0, result.stderr
    assert ": 0 new, 0 gone." in result.stdout
    assert (sample_baselines / KNOWN_GAPS_FILE_NAME).read_text() == first


def _add_navigation_header(form: dict[str, Any]) -> None:
    form["requests"]["navigation"]["headers"].append("x-changed: yes")


def _change_legacy_version(form: dict[str, Any]) -> None:
    form["tls"]["legacy_version"] = "SSL 3.0"


def _change_window_update(form: dict[str, Any]) -> None:
    form["http2"]["preface"][1]["increment"] = 1


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("change", "kinds", "field"),
    [
        (_add_navigation_header, ["navigation"], "request.headers.x-changed"),
        (_change_legacy_version, _ALL_KINDS, "tls.legacy_version"),
        (_change_window_update, _ALL_KINDS, "http2.preface.WINDOW_UPDATE.increment"),
    ],
)
def test_a_deliberate_change_to_a_sample_baseline_fails_the_run(
    sample_baselines: Path, change_baseline: ChangeBaseline, change: Any, kinds: list[str], field: str
) -> None:
    listed = find_gaps(load_baselines(sample_baselines))
    change_baseline("http2", change)

    found = mismatches(find_gaps(load_baselines(sample_baselines)), listed)

    assert found
    assert [(gap.release, gap.case, gap.protocol, gap.field) for gap in found.unlisted] == [
        (SAMPLE_RELEASE, kind, HTTP2, field) for kind in kinds
    ]
    report = str(found)
    assert f"Chrome {SAMPLE_RELEASE}, {kinds[0]}, {HTTP2}, {field}" in report
    assert "Baseline: " in report
    assert "mitto:    " in report


@pytest.mark.usefixtures("native_library")
def test_a_listed_gap_that_no_longer_exists_fails_the_run(
    sample_baselines: Path, change_baseline: ChangeBaseline
) -> None:
    listed_for_the_original = find_gaps(load_baselines(sample_baselines))
    change_baseline("http2", _add_navigation_header)
    listed = find_gaps(load_baselines(sample_baselines))
    assert len(listed) == len(listed_for_the_original) + 1

    def undo(form: dict[str, Any]) -> None:
        form["requests"]["navigation"]["headers"].remove("x-changed: yes")

    change_baseline("http2", undo)
    found = mismatches(find_gaps(load_baselines(sample_baselines)), listed)

    assert found
    assert found.unlisted == []
    assert [(gap.case, gap.protocol, gap.field, gap.mitto) for gap in found.gone] == [
        ("navigation", HTTP2, "request.headers.x-changed", None)
    ]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("protocol", "stem"), [(HTTP1, "http1"), (HTTP2, "http2"), (HTTP3, "http3")])
def test_a_baseline_recorded_from_the_native_core_has_no_gaps(tmp_path: Path, protocol: str, stem: str) -> None:
    persona = load_persona(CHROME_157_WINDOWS)
    with (
        CaptureServer() as server,
        Session(
            persona=CHROME_157_WINDOWS,
            _settings=PrivateSettings(
                ignore_certificate_errors_spki_list=[server.spki_hash],
                origins_to_force_quic_on=[server.authority(HTTP3)],
            ),
        ) as session,
    ):
        for kind in RequestKind:
            session.get(f"https://{server.authority(protocol)}{KIND_PATHS[kind]}", kind=kind.value)
    release = tmp_path / persona.release
    release.mkdir()
    (release / f"{stem}.json").write_text(dumps_normal_form(normal_form(server.recording(protocol))))

    baselines = load_baselines(tmp_path)

    assert [baseline.case for baseline in baselines] == list(_ALL_KINDS)
    assert find_gaps(baselines) == []


@pytest.mark.usefixtures("native_library")
def test_the_native_core_reaches_the_capture_server_over_the_baselines_protocol(sample_baselines: Path) -> None:
    fields = {(gap.protocol, gap.field) for gap in find_gaps(load_baselines(sample_baselines))}

    assert (HTTP1, "protocol") not in fields
    assert (HTTP2, "protocol") not in fields
    assert not any(field == "response" for _, field in fields)


def test_the_command_needs_a_native_library_and_leaves_the_list_alone(sample_baselines: Path) -> None:
    known = sample_baselines / KNOWN_GAPS_FILE_NAME
    known.write_text("[]\n")

    result = regenerate(sample_baselines, **{LIBRARY_ENV_VAR: str(sample_baselines / "no_such_library")})

    assert result.returncode == 1
    assert LIBRARY_ENV_VAR in result.stderr
    assert known.read_text() == "[]\n"


def test_the_command_refuses_a_bad_baseline(sample_baselines: Path) -> None:
    (sample_baselines / SAMPLE_RELEASE / "http2.json").write_text("not json")

    result = regenerate(sample_baselines)

    assert result.returncode == 1
    assert "http2.json" in result.stderr


def test_the_command_refuses_a_chrome_release_with_no_persona_and_leaves_the_list_alone(
    sample_baselines: Path,
) -> None:
    known = sample_baselines / KNOWN_GAPS_FILE_NAME
    known.write_text("[]\n")
    (sample_baselines / SAMPLE_RELEASE).rename(sample_baselines / "154")

    result = regenerate(sample_baselines)

    assert result.returncode == 1
    assert "Chrome 154" in result.stderr
    assert known.read_text() == "[]\n"


def test_the_command_refuses_a_bad_list_and_leaves_it_alone(tmp_path: Path) -> None:
    known = tmp_path / KNOWN_GAPS_FILE_NAME
    known.write_text("not a list")

    result = regenerate(tmp_path)

    assert result.returncode == 1
    assert "known-gaps" in result.stderr
    assert known.read_text() == "not a list"


def test_the_command_writes_an_empty_list_when_there_are_no_baselines(tmp_path: Path) -> None:
    result = regenerate(tmp_path)

    assert result.returncode == 0
    assert json.loads((tmp_path / KNOWN_GAPS_FILE_NAME).read_text()) == []
