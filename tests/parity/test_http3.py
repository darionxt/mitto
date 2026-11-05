"""Parity Checks over HTTP/3, as far as they go before the native core can reach HTTP/3.

The comparison and the capture server's HTTP/3 meet here with aioquic standing in for the native core, so this runs on
every machine. The tests that take the native_library fixture say what the native core itself does today.
"""

import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from mitto._native import RequestError
from tests.capture import samples
from tests.capture.http3_client import Http3Client
from tests.parity.conftest import SAMPLE_RELEASE, sample_form
from tools.capture import HTTP3, KIND_PATHS, CaptureServer, RequestKind, normal_form
from tools.capture.recording import Json
from tools.parity import (
    KNOWN_GAPS_FILE_NAME,
    dumps_known_gaps,
    find_gaps,
    fingerprint,
    load_baselines,
    loads_known_gaps,
)
from tools.parity.check import record

_REPOSITORY = Path(__file__).parents[2]
_ALL_KINDS = ["navigation", "fetch", "script", "image"]
_CONNECTION_REFUSED = -102


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

    assert [(baseline.protocol, baseline.kind.value) for baseline in baselines] == [("h3", kind) for kind in _ALL_KINDS]
    assert sorted(baselines[0].fingerprint) == ["format", "http3", "protocol", "quic", "request", "tls"]


def test_a_recording_like_the_http3_baseline_has_no_gaps(http3_baselines: Path) -> None:
    def recorder(protocol: str, kind: RequestKind) -> dict[str, Json]:
        return fingerprint(sample_form("http3"), kind)

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
    def recorder(protocol: str, kind: RequestKind) -> dict[str, Json]:
        form = sample_form("http3")
        change(form)
        return fingerprint(form, kind)

    gaps = find_gaps(load_baselines(http3_baselines), recorder)

    assert {gap.field for gap in gaps} == {field}
    assert {gap.protocol for gap in gaps} == {"h3"}


def test_what_the_capture_server_records_over_http3_goes_through_the_comparison(http3_baselines: Path) -> None:
    def recorder(protocol: str, kind: RequestKind) -> dict[str, Json]:
        with CaptureServer() as server, Http3Client(server.port(protocol)) as client:
            client.get(KIND_PATHS[kind])
            return fingerprint(normal_form(server.recording(protocol)), kind)

    gaps = find_gaps(load_baselines(http3_baselines), recorder)

    fields = {gap.field for gap in gaps}
    assert {gap.protocol for gap in gaps} == {"h3"}
    assert {gap.kind for gap in gaps} == set(_ALL_KINDS)
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
def test_the_native_core_gets_no_response_over_http3_yet() -> None:
    """The native core asks for TCP on the UDP port. Forcing QUIC on the origin comes with QUIC parity."""
    with pytest.raises(RequestError) as caught:
        record(HTTP3, RequestKind.FETCH)

    assert caught.value.net_error == _CONNECTION_REFUSED


@pytest.mark.usefixtures("native_library")
def test_an_http3_baseline_is_one_gap_per_request_kind_for_now(http3_baselines: Path) -> None:
    gaps = find_gaps(load_baselines(http3_baselines))

    assert [(gap.kind, gap.protocol, gap.field, gap.mitto) for gap in gaps] == [
        (kind, "h3", "response", f"net error {_CONNECTION_REFUSED}") for kind in _ALL_KINDS
    ]


@pytest.mark.usefixtures("native_library")
def test_a_regenerated_list_holds_those_gaps_and_stays_the_same(http3_baselines: Path) -> None:
    command = [sys.executable, "-m", "tools.parity", "--baselines", str(http3_baselines)]

    first = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)
    text = (http3_baselines / KNOWN_GAPS_FILE_NAME).read_text()
    second = subprocess.run(command, cwd=_REPOSITORY, capture_output=True, text=True, check=False)

    assert first.returncode == 0, first.stderr
    assert "Wrote 4 gaps" in first.stdout
    assert len(loads_known_gaps(text)) == 4
    assert ": 0 new, 0 gone." in second.stdout
    assert (http3_baselines / KNOWN_GAPS_FILE_NAME).read_text() == text
