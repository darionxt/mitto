"""Tests that run the native core's C test program against a local server, as the sanitizer jobs in CI do.

A sanitizer report ends the program with an error. These run with a sanitized program because a Python process cannot
load a sanitized library: the sanitizers' runtime is linked into the program that starts, not into the library.
"""

import json
import os
import subprocess
from pathlib import Path

import pytest

from mitto._native import SETTINGS_VERSION
from tools.capture import HTTP1, HTTP2, HTTP3, CaptureServer

_PROGRAM_ENV_VAR = "MITTO_CORE_TEST"
_ERR_INVALID_ARGUMENT = -4
_ERR_CERT_AUTHORITY_INVALID = -202
_TIMEOUT_SECONDS = 120


@pytest.fixture
def program() -> Path:
    """Return the test program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no test program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, url: str, settings: dict[str, object]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [program, url, json.dumps(settings)], capture_output=True, text=True, timeout=_TIMEOUT_SECONDS
    )
    # A report can come after the program has done its work, and it exits with the same code as the program's failures.
    assert "Sanitizer" not in result.stderr, result.stderr
    return result


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_over_each_protocol(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        result = _run(
            program,
            server.url(protocol),
            {
                "version": SETTINGS_VERSION,
                "ignore_certificate_errors_spki_list": [server.spki_hash],
                "accept_language": "en-US,en;q=0.9",
                "origins_to_force_quic_on": [server.authority(HTTP3)] if protocol == HTTP3 else [],
            },
        )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == f"status 200 over {protocol}"


def test_settings_that_are_refused(program: Path) -> None:
    result = _run(program, "http://127.0.0.1:9/", {"version": SETTINGS_VERSION, "unknown": True})

    assert result.returncode == 1
    assert f"net error {_ERR_INVALID_ARGUMENT}" in result.stderr


def test_a_request_that_fails(program: Path) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(HTTP2), {"version": SETTINGS_VERSION})

    assert result.returncode == 1
    assert f"net error {_ERR_CERT_AUTHORITY_INVALID}" in result.stderr
