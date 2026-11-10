"""Tests of tools/workspace/v8_settings.py without a checkout: what it takes from an official builder, and what it reports."""

import json
from pathlib import Path
from typing import Any

import pytest

from tools.workspace import export, v8_settings


def _builder(src: Path, host: export.Host, args: dict[str, object]) -> None:
    path = src / v8_settings.OFFICIAL_ARGS.format(host=host.name)
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps({"gn_args": args}))


def _graph(defines: list[str], snapshot: list[str]) -> dict[str, Any]:
    graph: dict[str, Any] = {label: {"defines": defines} for label in v8_settings.COMPILED}
    graph[v8_settings.SNAPSHOT] = {"args": snapshot}
    return graph


def test_an_official_builders_args_come_without_those_that_change_nothing_in_v8(tmp_path: Path) -> None:
    _builder(
        tmp_path,
        export.WINDOWS,
        {"is_official_build": True, "symbol_level": 1, "target_cpu": "x64", "target_os": "win", "use_siso": True},
    )

    args = v8_settings.official_args(tmp_path, export.WINDOWS)

    assert args == 'is_official_build = true\ntarget_cpu = "x64"\ntarget_os = "win"\n'


def test_a_builder_that_is_not_official_is_refused(tmp_path: Path) -> None:
    _builder(tmp_path, export.LINUX, {"target_cpu": "x64"})

    with pytest.raises(export.ExportError, match="is_official_build"):
        v8_settings.official_args(tmp_path, export.LINUX)


def test_a_missing_builder_is_named(tmp_path: Path) -> None:
    with pytest.raises(export.ExportError, match="mac-official"):
        v8_settings.official_args(tmp_path, export.MAC)


def test_defines_that_differ_on_purpose_are_not_differences() -> None:
    runtime = _graph(["V8_COMPRESS_POINTERS"], ["./mksnapshot"])
    chrome = _graph(["OFFICIAL_BUILD", "V8_COMPRESS_POINTERS"], ["./mksnapshot"])

    assert v8_settings.differences(runtime, chrome) == []


def test_every_other_difference_is_reported() -> None:
    runtime = _graph(["V8_LOGGING_LEVEL=1"], ["./mksnapshot"])
    chrome = _graph(["V8_LOGGING_LEVEL=0"], ["./mksnapshot", "--turbo-profiling-input", "x64.profile"])

    found = v8_settings.differences(runtime, chrome)

    assert len(found) == 2 * len(v8_settings.COMPILED) + 1
    assert "//v8:v8_base_without_compiler: only the Runtime defines V8_LOGGING_LEVEL=1" in found
    assert "//v8:cppgc_base: only Chrome defines V8_LOGGING_LEVEL=0" in found
    assert found[-1].startswith(f"{v8_settings.SNAPSHOT} runs with ['./mksnapshot'], and in Chrome with")
