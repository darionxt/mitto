"""Parity Checks over HTTP/3.

The comparison and the capture server's HTTP/3 meet here with aioquic standing in for the native core, so this runs on
every machine. The tests that take the native_library fixture go through the native core itself.
"""

import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from mitto._native import RequestError, Session
from tests.capture import samples
from tests.capture.http3_client import Http3Client
from tests.parity.conftest import SAMPLE_RELEASE, sample_form
from tools.capture import HTTP3, KIND_PATHS, CaptureServer, RequestKind, normal_form
from tools.capture.recording import Json
from tools.parity import (
    KNOWN_GAPS_FILE_NAME,
    Baseline,
    dumps_known_gaps,
    find_gaps,
    fingerprint,
    load_baselines,
    loads_known_gaps,
)

_REPOSITORY = Path(__file__).parents[2]
_ALL_KINDS = ["navigation", "fetch", "script", "image"]
_CONNECTION_REFUSED = -102
_QUIC_PROTOCOL_ERROR = -356


@pytest.fixture
def http3_baselines(tmp_path: Path) -> Path:
    """Return a directory of Baselines with only the HTTP/3 sample, and no known-gaps list."""
    release = tmp_path / SAMPLE_RELEASE
    release.mkdir()
    shutil.copy(samples.DATA / "chrome_http3.normal.json", release / "http3.json")
    return tmp_path


def _parameters(form: dict[str, Any]) -> list[dict[str, Any]]:
    extension = next(item for item in form["tls"]["extensions"] if item["type"] == "quic_transport_parameters")
    return extension["parameters"]


def _change_datagram_size(form: dict[str, Any]) -> None:
    form["quic"]["first_datagram_ip_size"] = 1400


def _change_a_parameter(form: dict[str, Any]) -> None:
    next(item for item in _parameters(form) if item["type"] == "max_idle_timeout")["value"] = 1


def _drop_a_parameter(form: dict[str, Any]) -> None:
    _parameters(form).remove({"type": "GREASE"})


def _change_other_versions(form: dict[str, Any]) -> None:
    next(item for item in _parameters(form) if item["type"] == "version_information")["others"] = ["QUIC v1"]


def _change_a_setting(form: dict[str, Any]) -> None:
    settings = form["http3"]["control"][0]["settings"]
    settings[0] = "QPACK_MAX_TABLE_CAPACITY: 1"


def _drop_a_control_frame(form: dict[str, Any]) -> None:
    form["http3"]["control"].remove({"type": "GREASE"})


def _change_a_stream(form: dict[str, Any]) -> None:
    next(item for item in form["http3"]["streams"] if item["type"] == "qpack_encoder")["stream"] = 6


def _change_a_priority_update(form: dict[str, Any]) -> None:
    form["requests"]["navigation"]["priority_updates"] = ["u=7"]


def _change_a_header(form: dict[str, Any]) -> None:
    headers = form["requests"]["navigation"]["headers"]
    headers[headers.index(":scheme: https")] = ":scheme: http"


def test_each_request_kind_in_a_file_is_one_http3_baseline(http3_baselines: Path) -> None:
    baselines = load_baselines(http3_baselines)

    assert [(baseline.protocol, baseline.case) for baseline in baselines] == [("h3", kind) for kind in _ALL_KINDS]
    assert sorted(baselines[0].fingerprint) == ["format", "http3", "protocol", "quic", "request", "tls"]


def test_a_recording_like_the_http3_baseline_has_no_gaps(http3_baselines: Path) -> None:
    def recorder(baseline: Baseline) -> dict[str, Json]:
        return fingerprint(sample_form("http3"), baseline.case)

    assert find_gaps(load_baselines(http3_baselines), recorder) == []


@pytest.mark.parametrize(
    ("change", "field"),
    [
        (_change_datagram_size, "quic.first_datagram_ip_size"),
        (_change_a_parameter, "tls.extensions.quic_transport_parameters.parameters.max_idle_timeout.value"),
        (_drop_a_parameter, "tls.extensions.quic_transport_parameters.parameters.GREASE"),
        (_change_other_versions, "tls.extensions.quic_transport_parameters.parameters.version_information.others"),
        (_change_a_setting, "http3.control.SETTINGS.settings.QPACK_MAX_TABLE_CAPACITY"),
        (_drop_a_control_frame, "http3.control.GREASE"),
        (_change_a_stream, "http3.streams.qpack_encoder.stream"),
        (_change_a_priority_update, "request.priority_updates"),
        (_change_a_header, "request.headers.:scheme"),
    ],
)
def test_each_difference_in_http3_is_its_own_gap(
    http3_baselines: Path, change: Callable[[dict[str, Any]], None], field: str
) -> None:
    def recorder(baseline: Baseline) -> dict[str, Json]:
        form = sample_form("http3")
        change(form)
        return fingerprint(form, baseline.case)

    gaps = find_gaps(load_baselines(http3_baselines), recorder)

    assert {gap.field for gap in gaps} == {field}
    assert {gap.protocol for gap in gaps} == {"h3"}


def test_what_the_capture_server_records_over_http3_goes_through_the_comparison(http3_baselines: Path) -> None:
    def recorder(baseline: Baseline) -> dict[str, Json]:
        with CaptureServer() as server, Http3Client(server.port(baseline.protocol)) as client:
            client.get(KIND_PATHS[RequestKind(baseline.case)])
            return fingerprint(normal_form(server.recording(baseline.protocol)), baseline.case)

    gaps = find_gaps(load_baselines(http3_baselines), recorder)

    fields = {gap.field for gap in gaps}
    assert {gap.protocol for gap in gaps} == {"h3"}
    assert {gap.case for gap in gaps} == set(_ALL_KINDS)
    # The client is not Chrome, so this finds differences, each in a field of its own.
    assert "quic.first_datagram_ip_size" in fields
    assert "tls.extensions.trust_anchors" in fields
    assert "http3.control.SETTINGS.settings.H3_DATAGRAM" in fields
    assert "request.headers.sec-fetch-site" in fields
    assert "response" not in fields
    assert "protocol" not in fields
    # The command line writes the gaps to the known-gaps list, and later runs read them back.
    assert loads_known_gaps(dumps_known_gaps(gaps)) == gaps


@pytest.mark.usefixtures("native_library")
def test_a_session_learns_of_quic_from_alt_svc_unless_it_is_told_to_force_it() -> None:
    """Without the setting, the native core asks for TCP on the UDP port, as Chrome does without the flag."""
    with CaptureServer() as server, Session(ignore_certificate_errors_spki_list=[server.spki_hash]) as session:
        url = f"https://{server.authority(HTTP3)}{KIND_PATHS[RequestKind.FETCH]}"
        with pytest.raises(RequestError) as caught:
            session.request(url)

    assert caught.value.net_error == _CONNECTION_REFUSED


@pytest.mark.usefixtures("native_library")
def test_a_session_reaches_a_forced_origin_over_http3() -> None:
    with (
        CaptureServer() as server,
        Session(
            ignore_certificate_errors_spki_list=[server.spki_hash], origins_to_force_quic_on=[server.authority(HTTP3)]
        ) as session,
    ):
        response = session.request(f"https://{server.authority(HTTP3)}{KIND_PATHS[RequestKind.FETCH]}")

    assert (response.status, response.protocol) == (200, "h3")


@pytest.mark.usefixtures("native_library")
def test_a_session_that_forces_every_origin_reaches_the_server_over_http3() -> None:
    with (
        CaptureServer() as server,
        Session(ignore_certificate_errors_spki_list=[server.spki_hash], origins_to_force_quic_on=["*"]) as session,
    ):
        response = session.request(f"https://{server.authority(HTTP3)}{KIND_PATHS[RequestKind.FETCH]}")

    assert (response.status, response.protocol) == (200, "h3")


@pytest.mark.usefixtures("native_library")
def test_forcing_quic_does_not_skip_the_check_of_the_certificate() -> None:
    with (
        CaptureServer() as server,
        Session(origins_to_force_quic_on=[server.authority(HTTP3)]) as session,
        pytest.raises(RequestError) as caught,
    ):
        session.request(f"https://{server.authority(HTTP3)}{KIND_PATHS[RequestKind.FETCH]}")

    # QUIC reports a certificate it does not accept as this one error.
    assert caught.value.net_error == _QUIC_PROTOCOL_ERROR


@pytest.mark.usefixtures("native_library")
def test_the_native_core_differs_from_chrome_in_no_quic_or_http3_field(http3_baselines: Path) -> None:
    """The sample is Chrome on macOS, so the Persona's headers and the trust anchors can differ, and nothing below them."""
    fields = {gap.field for gap in find_gaps(load_baselines(http3_baselines))}

    assert not {
        field
        for field in fields
        if field in ("protocol", "response")
        or field.startswith(("quic.", "http3.", "tls.extensions.quic_transport_parameters"))
    }


@pytest.mark.usefixtures("native_library")
def test_a_regenerated_list_stays_the_same_over_http3(http3_baselines: Path) -> None:
    command = [sys.executable, "-m", "tools.parity", "--baselines", str(http3_baselines)]

    first = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)
    assert first.returncode == 0, first.stderr
    text = (http3_baselines / KNOWN_GAPS_FILE_NAME).read_text()
    second = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)
    assert second.returncode == 0, second.stderr

    assert ": 0 new, 0 gone." in second.stdout
    assert (http3_baselines / KNOWN_GAPS_FILE_NAME).read_text() == text
