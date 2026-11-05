"""The capture kit against a real Chrome. It only runs where MITTO_CHROME names a Chrome executable.

Chrome runs headless, so this needs no display and takes no one's screen. It starts from a new user data directory
every time, and it reaches nothing but the capture server's test page.
"""

import json
import os
import subprocess
import tempfile

import pytest

from tools.capture import HTTP1, HTTP2, CaptureServer, normal_form
from tools.capture.recording import Json

_CHROME_ENV_VAR = "MITTO_CHROME"
_PAGE_TIMEOUT_SECONDS = 60.0
_EXIT_TIMEOUT_SECONDS = 10.0


@pytest.fixture(scope="module")
def chrome() -> str:
    path = os.environ.get(_CHROME_ENV_VAR)
    if not path:
        pytest.skip(f"set {_CHROME_ENV_VAR} to a Chrome executable to run this test")
    return path


def record(chrome: str, protocol: str) -> dict[str, Json]:
    """Load the test page in a Chrome with a fresh user data directory, and return the normal form of what it sent."""
    with (
        CaptureServer() as server,
        tempfile.TemporaryDirectory(prefix="mitto-capture-", ignore_cleanup_errors=True) as user_data,
    ):
        process = subprocess.Popen(
            [
                chrome,
                "--headless=new",
                f"--user-data-dir={user_data}",
                f"--ignore-certificate-errors-spki-list={server.spki_hash}",
                "--no-first-run",
                "--no-default-browser-check",
                server.url(protocol),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            assert server.wait_for_page(protocol, _PAGE_TIMEOUT_SECONDS), "the test page did not finish"
        finally:
            process.terminate()
            try:
                process.wait(_EXIT_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        return normal_form(server.recording(protocol))


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_two_recordings_have_the_same_normal_form(chrome: str, protocol: str) -> None:
    first = record(chrome, protocol)
    second = record(chrome, protocol)

    assert json.dumps(first, indent=2) == json.dumps(second, indent=2)
    assert first["protocol"] == protocol
    requests = first["requests"]
    assert isinstance(requests, dict)
    assert list(requests) == ["navigation", "fetch", "script", "image"]
