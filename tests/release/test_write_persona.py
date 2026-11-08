"""Tests of the script that writes a Persona's file from the Baselines of its Chrome Release. They need no native library."""

import dataclasses
import json
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import cast

import pytest

from mitto._machine import MACHINE_HINTS
from mitto._persona import MACHINE_PLACEHOLDER, WINDOWS_PERSONAS
from tools.capture import HTTP1, HTTP2
from tools.capture.recording import Json
from tools.parity import BASELINES_DIRECTORY, Baseline, load_baselines
from tools.release import persona
from tools.release.persona import PersonaDataError, dumps_persona, persona_data, persona_path, write_persona

BASELINES = load_baselines(BASELINES_DIRECTORY)
ENGINE = max(WINDOWS_PERSONAS, key=int)


def changed(case: str, protocol: str, change: Callable[[list[str]], list[str]]) -> list[Baseline]:
    """Return the Baselines with the header lines of one Baseline of the Engine Release changed."""
    baselines: list[Baseline] = []
    for baseline in BASELINES:
        if (baseline.release, baseline.case, baseline.protocol) == (ENGINE, case, protocol):
            request = cast(dict[str, Json], baseline.fingerprint["request"])
            headers = change([str(line) for line in cast(list[Json], request["headers"])])
            fingerprint = {**baseline.fingerprint, "request": {**request, "headers": list[Json](headers)}}
            baseline = dataclasses.replace(baseline, fingerprint=fingerprint)
        baselines.append(baseline)
    return baselines


def replaced(name: str, value: str) -> Callable[[list[str]], list[str]]:
    """Return what gives the line of this name another value."""
    return lambda lines: [f"{name}: {value}" if line.partition(":")[0].lower() == name else line for line in lines]


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_the_file_of_each_persona_is_byte_for_byte_what_its_baselines_write(release: str) -> None:
    written = dumps_persona(persona_data(release, BASELINES)).encode()

    assert written == persona_path(release).read_bytes()


def test_writing_a_persona_writes_its_file_in_the_directory(tmp_path: Path) -> None:
    path = write_persona(ENGINE, BASELINES_DIRECTORY, tmp_path)

    assert path == tmp_path / f"chrome_{ENGINE}_windows.json"
    assert path.read_bytes() == persona_path(ENGINE).read_bytes()


def test_a_new_chrome_release_gets_the_release_of_its_directory(tmp_path: Path) -> None:
    new = str(int(ENGINE) + 1)
    shutil.copytree(BASELINES_DIRECTORY / ENGINE, tmp_path / "baselines" / new)
    (tmp_path / "personas").mkdir()

    path = write_persona(new, tmp_path / "baselines", tmp_path / "personas")

    assert path.name == f"chrome_{new}_windows.json"
    assert json.loads(path.read_text())["release"] == new


def test_a_chrome_release_with_no_baselines_is_an_error() -> None:
    new = str(int(ENGINE) + 1)

    with pytest.raises(PersonaDataError, match=f"Chrome {new} has no Baselines"):
        persona_data(new, BASELINES)


@pytest.mark.parametrize("case", ["client_hints_form", "client_hints_script", "image", "navigation"])
def test_a_recording_case_without_a_baseline_is_an_error(case: str) -> None:
    baselines = [baseline for baseline in BASELINES if baseline.case != case]

    with pytest.raises(PersonaDataError, match=f"no Baseline of {case} over"):
        persona_data(ENGINE, baselines)


@pytest.mark.parametrize(
    ("case", "value"),
    [
        ("fetch_post_json_same_origin", "application/json;charset=UTF-8"),
        ("navigation_form_multipart", "multipart/mixed; boundary=----WebKitFormBoundary<random>"),
    ],
)
def test_a_content_type_that_differs_from_the_personas_is_an_error(case: str, value: str) -> None:
    with pytest.raises(PersonaDataError, match=f"{case} sends Content-Type: "):
        persona_data(ENGINE, changed(case, HTTP1, replaced("content-type", value)))


@pytest.mark.parametrize("value", ["u=8, i", "u=1, x", "x"])
def test_a_priority_that_net_does_not_send_is_an_error(value: str) -> None:
    with pytest.raises(PersonaDataError, match="the priority of image over HTTP/2"):
        persona_data(ENGINE, changed("image", HTTP2, replaced("priority", value)))


def test_a_priority_header_that_is_not_there_is_the_default_urgency_and_not_incremental() -> None:
    def without_priority(lines: list[str]) -> list[str]:
        return [line for line in lines if not line.startswith("priority:")]

    data = persona_data(ENGINE, changed("image", HTTP2, without_priority))
    image = cast(dict[str, dict[str, Json]], data["kinds"])["image"]

    assert (image["priority"], image["priority_incremental"]) == ("LOWEST", False)


def test_a_hint_of_the_machine_that_a_machine_has_no_value_for_is_an_error(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(persona, "MACHINE_HINTS", persona.MACHINE_HINTS - {"rtt"})

    with pytest.raises(PersonaDataError, match="rtt follows the machine"):
        persona_data(ENGINE, BASELINES)


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_each_hint_that_follows_the_machine_holds_the_placeholder_and_nothing_else_does(release: str) -> None:
    kinds = cast(dict[str, dict[str, Json]], persona_data(release, BASELINES)["kinds"])
    for kind in kinds.values():
        lines = [str(line).partition(": ") for line in cast(list[Json], kind["headers"])]
        placeholders = {name.lower() for name, _, value in lines if value == MACHINE_PLACEHOLDER}
        hints = {name.lower() for name, _, _ in lines} & MACHINE_HINTS

        assert placeholders == hints


def test_a_hints_case_with_no_hint_of_its_own_is_an_error() -> None:
    (script,) = [b for b in BASELINES if (b.release, b.case, b.protocol) == (ENGINE, "script", HTTP1)]
    script_lines = [
        str(line) for line in cast(list[Json], cast(dict[str, Json], script.fingerprint["request"])["headers"])
    ]

    with pytest.raises(PersonaDataError, match="client_hints_script has no hint"):
        persona_data(ENGINE, changed("client_hints_script", HTTP1, lambda _lines: script_lines))
