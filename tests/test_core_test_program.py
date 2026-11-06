"""Tests that run the native core's C test program against a local server, as the sanitizer jobs in CI do.

A sanitizer report ends the program with an error. These run with a sanitized program because a Python process cannot
load a sanitized library: the sanitizers' runtime is linked into the program that starts, not into the library.
"""

import base64
import json
import os
import socket
import subprocess
from pathlib import Path

import pytest

from mitto._native import SETTINGS_VERSION
from tests.local_certificates import AiaServer, TlsServer, generate_intermediate, generate_leaf, generate_root, serving
from tests.local_proxy import LocalProxy
from tools.capture import (
    CASE_HOST,
    CASES,
    COOKIE_SET_PATH,
    HTTP1,
    HTTP2,
    HTTP3,
    KIND_PATHS,
    MAIN_CASES,
    CaptureServer,
    RequestKind,
    normal_form,
)
from tools.parity.replay import run_of, session_settings

_PROGRAM_ENV_VAR = "MITTO_CORE_TEST"
_ERR_INVALID_ARGUMENT = -4
_ERR_PROXY_AUTH_REQUESTED = -127
_ERR_CERT_AUTHORITY_INVALID = -202
_ERR_INVALID_URL = -300
_TIMEOUT_SECONDS = 120
_HOST = "localhost"
_FETCH_PATH = KIND_PATHS[RequestKind.FETCH]


@pytest.fixture
def program() -> Path:
    """Return the test program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no test program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(
    program: Path, url: str, settings: dict[str, object], scenario: str = "fetch"
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [program, url, json.dumps(settings), scenario], capture_output=True, text=True, timeout=_TIMEOUT_SECONDS
    )
    # A report can come after the program has done its work, and it exits with the same code as the program's failures.
    assert "Sanitizer" not in result.stderr, result.stderr
    return result


def _settings(server: CaptureServer, protocol: str) -> dict[str, object]:
    return {
        "version": SETTINGS_VERSION,
        "ignore_certificate_errors_spki_list": [server.spki_hash],
        "accept_language": "en-US,en;q=0.9",
        "origins_to_force_quic_on": [server.authority(HTTP3)] if protocol == HTTP3 else [],
    }


@pytest.mark.parametrize("scenario", ["fetch", "fetch-with-file-descriptor"])
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_over_each_protocol(program: Path, protocol: str, scenario: str) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol), _settings(server, protocol), scenario)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == f"status 200 over {protocol}"


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_a_request_through_a_proxy_that_asks_for_credentials(program: Path, protocol: str) -> None:
    authorization = "Basic " + base64.b64encode(b"user:password").decode()
    with serving(LocalProxy(authorization=authorization)) as proxy, CaptureServer() as server:
        settings = {**_settings(server, protocol), "proxy": f"http://user:password@127.0.0.1:{proxy.port}"}
        result = _run(program, server.url(protocol, host=CASE_HOST), settings)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == f"status 200 over {protocol}"
    assert proxy.requests[1].header("Proxy-Authorization") == authorization


def test_a_request_through_a_proxy_that_refuses_its_credentials(program: Path) -> None:
    with serving(LocalProxy(authorization="Basic other")) as proxy, CaptureServer() as server:
        settings = {**_settings(server, HTTP2), "proxy": f"http://user:password@127.0.0.1:{proxy.port}"}
        result = _run(program, server.url(HTTP2, host=CASE_HOST), settings)

    assert result.returncode == 1
    assert f"net error {_ERR_PROXY_AUTH_REQUESTED}" in result.stderr
    assert len(proxy.requests) == 2


def _redirect_url(server: CaptureServer, protocol: str) -> tuple[str, dict[str, object]]:
    """Return the first of the capture server's redirects over a protocol, and the settings that reach it."""
    run = run_of("redirect_fetch_1", protocol)
    assert run is not None
    path = next(case.path for case in CASES if case.name == "redirect_fetch_1")
    return server.url(run.route, host=run.host, path=path), {
        "version": SETTINGS_VERSION,
        **session_settings(server, run),
    }


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_that_follows_redirects(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        url, settings = _redirect_url(server, protocol)
        result = _run(program, url, settings)

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert [line.split(" from ")[0] for line in lines[:3]] == ["redirect 302"] * 3
    assert lines[0] == f"redirect 302 from {url}"
    assert lines[3] == f"status 200 over {protocol}"


@pytest.mark.parametrize("scenario", ["navigation-from-a-page", "cors-from-a-page", "no-cors-with-no-initiator"])
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_from_a_page_that_follows_redirects(program: Path, protocol: str, scenario: str) -> None:
    # The rules of each hop run, under the sanitizers. tests/test_initiator.py has what they send.
    with CaptureServer() as server:
        url, settings = _redirect_url(server, protocol)
        result = _run(program, url, settings, scenario)

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert [line.split(" from ")[0] for line in lines[:3]] == ["redirect 302"] * 3
    assert lines[3] == f"status 200 over {protocol}"


@pytest.mark.parametrize("scenario", ["navigation-from-a-page", "cors-from-a-page", "no-cors-with-no-initiator"])
def test_a_url_that_is_not_valid_from_a_page_fails_as_any_other(program: Path, scenario: str) -> None:
    result = _run(program, "not a url", {"version": SETTINGS_VERSION}, scenario)

    assert result.returncode != 0
    assert f"net error {_ERR_INVALID_URL}" in result.stderr + result.stdout


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_that_does_not_follow_redirects(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        url, settings = _redirect_url(server, protocol)
        result = _run(program, url, settings, "fetch-without-redirects")

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[:2] == [f"status 302 over {protocol}", f"url: {url}"]
    assert lines[-1] == "body: 0 bytes"


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_that_is_cancelled_at_a_redirect_leaves_the_session_working(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        url, settings = _redirect_url(server, protocol)
        result = _run(program, url, settings, "cancel-at-redirect")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == "ended"
    assert f"status 200 over {protocol}" in result.stdout


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_session_destroyed_at_a_redirect(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        url, settings = _redirect_url(server, protocol)
        result = _run(program, url, settings, "destroy-session-at-redirect")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["ended"]


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_body_goes_out_after_its_caller_has_reused_the_memory(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol, path=_FETCH_PATH), _settings(server, protocol), "post")
        form = normal_form(server.recording(protocol), MAIN_CASES)

    assert result.returncode == 0, result.stderr
    requests = form["requests"]
    assert isinstance(requests, dict)
    request = requests["fetch"]
    assert isinstance(request, dict)
    assert request["body"] == "the body of a request"


@pytest.mark.parametrize("scenario", ["cancel-at-once", "cancel-after-response"])
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_that_is_cancelled_leaves_the_session_working(program: Path, protocol: str, scenario: str) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol), _settings(server, protocol), scenario)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == "cancelled"
    assert f"status 200 over {protocol}" in result.stdout


@pytest.mark.parametrize("scenario", ["release-at-once", "release-after-response"])
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_request_that_is_released_running_leaves_the_session_working(
    program: Path, protocol: str, scenario: str
) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol), _settings(server, protocol), scenario)

    assert result.returncode == 0, result.stderr
    assert f"status 200 over {protocol}" in result.stdout


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_session_destroyed_with_a_request_running(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol), _settings(server, protocol), "destroy-session-while-running")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["cancelled"]


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_process_that_exits_with_a_request_running(program: Path, protocol: str) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(protocol), _settings(server, protocol), "exit-while-running")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["exiting"]


def test_the_calls_for_cookies(program: Path) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(HTTP1, path=COOKIE_SET_PATH), _settings(server, HTTP1), "cookies")

    assert result.returncode == 0, result.stderr
    listed = [line for line in result.stdout.splitlines() if line.startswith("cookies")]
    names = [[cookie["name"] for cookie in json.loads(line.partition(": ")[2])["cookies"]] for line in listed]
    assert names == [["mitto"], ["mitto", "added"], ["mitto"]]
    assert "deleted 1" in result.stdout


def test_settings_that_are_refused(program: Path) -> None:
    result = _run(program, "http://127.0.0.1:9/", {"version": SETTINGS_VERSION, "unknown": True})

    assert result.returncode == 1
    assert f"net error {_ERR_INVALID_ARGUMENT}" in result.stderr


def test_a_request_that_fails(program: Path) -> None:
    with CaptureServer() as server:
        result = _run(program, server.url(HTTP2), {"version": SETTINGS_VERSION})

    assert result.returncode == 1
    assert f"net error {_ERR_CERT_AUTHORITY_INVALID}" in result.stderr


@pytest.mark.parametrize(
    ("scenario", "scheme", "name"),
    [
        # Over https:// a server that never answers never finishes the TLS handshake, which the connect timer runs through.
        ("connect-timeout", "https", "ERR_CONNECTION_TIMED_OUT"),
        ("read-timeout", "http", "ERR_TIMED_OUT"),
        ("total-timeout", "http", "ERR_TIMED_OUT"),
    ],
)
def test_a_timer_that_runs_out(program: Path, scenario: str, scheme: str, name: str) -> None:
    # The system accepts the connection, and nothing here reads from it.
    with socket.create_server(("127.0.0.1", 0)) as listener:
        url = f"{scheme}://127.0.0.1:{listener.getsockname()[1]}/"
        result = _run(program, url, {"version": SETTINGS_VERSION}, scenario)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [f"timed out with {name}"]


def test_a_missing_intermediate_is_fetched_from_its_aia_url(program: Path, tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(intermediate.der)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, _HOST, aia.url))) as server,
    ):
        settings: dict[str, object] = {"version": SETTINGS_VERSION, "additional_trust_anchors": [root.pem]}
        result = _run(program, f"https://{_HOST}:{server.server_port}/", settings)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == "status 200 over http/1.1"


def _run_until_told(program: Path, tmp_path: Path, scenario: str) -> subprocess.CompletedProcess[str]:
    """Run a scenario, and let it go on once its request is waiting for a certificate fetch."""
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(hang=True)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, _HOST, aia.url))) as server,
    ):
        settings = {"version": SETTINGS_VERSION, "additional_trust_anchors": [root.pem]}
        url = f"https://{_HOST}:{server.server_port}/"
        args = [program, url, json.dumps(settings), scenario]
        with subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        ) as process:
            fetched = aia.requested.wait(_TIMEOUT_SECONDS)
            stdout, stderr = process.communicate("go", timeout=_TIMEOUT_SECONDS)
        aia.released.set()

    assert fetched, stderr
    assert "Sanitizer" not in stderr, stderr
    return subprocess.CompletedProcess(args, process.returncode, stdout, stderr)


def test_a_session_destroyed_during_a_certificate_fetch(program: Path, tmp_path: Path) -> None:
    result = _run_until_told(program, tmp_path, "destroy-session-when-told")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["cancelled"]


def test_a_process_that_exits_during_a_certificate_fetch(program: Path, tmp_path: Path) -> None:
    result = _run_until_told(program, tmp_path, "exit-when-told")

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["exiting"]
