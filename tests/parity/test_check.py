"""Tests of loading Baselines and finding gaps. They need no native library: a stand-in makes the requests."""

from pathlib import Path
from typing import Any

import pytest

from mitto._native import RequestError
from tests.parity.conftest import SAMPLE_RELEASE, ChangeBaseline, sample_form
from tools.capture import RequestKind
from tools.capture.recording import Json
from tools.parity import Baseline, BaselineError, Gap, find_gaps, fingerprint, load_baselines, mismatches

ALL_KINDS = [RequestKind.NAVIGATION, RequestKind.FETCH, RequestKind.SCRIPT, RequestKind.IMAGE]


def identical_to_the_sample(baseline: Baseline) -> dict[str, Json]:
    """Record what the unchanged sample Baseline holds, so that there is no gap until a Baseline changes."""
    return fingerprint(sample_form("http2" if baseline.protocol == "h2" else "http1"), baseline.kind)


def test_each_request_kind_in_a_file_is_one_baseline(sample_baselines: Path) -> None:
    baselines = load_baselines(sample_baselines)

    assert [(baseline.release, baseline.protocol, baseline.kind) for baseline in baselines] == [
        *((SAMPLE_RELEASE, "http/1.1", kind) for kind in ALL_KINDS),
        *((SAMPLE_RELEASE, "h2", kind) for kind in ALL_KINDS),
    ]


def test_a_baseline_holds_its_connection_and_its_own_request(sample_baselines: Path) -> None:
    form = sample_form("http2")
    baseline = load_baselines(sample_baselines)[4]

    assert baseline.kind is RequestKind.NAVIGATION
    assert baseline.fingerprint == {
        "format": form["format"],
        "protocol": "h2",
        "tls": form["tls"],
        "http2": form["http2"],
        "request": form["requests"]["navigation"],
    }


def test_a_new_top_level_field_stays_in_the_fingerprint() -> None:
    form: dict[str, Json] = {"protocol": "h3", "quic": {"version": 1}, "requests": {"image": {"headers": []}}}

    assert fingerprint(form, RequestKind.IMAGE) == {
        "protocol": "h3",
        "quic": {"version": 1},
        "request": {"headers": []},
    }


def test_a_request_kind_missing_from_a_recording_has_no_request() -> None:
    assert fingerprint({"protocol": "h2", "requests": {}}, RequestKind.IMAGE) == {"protocol": "h2", "request": None}


def test_a_directory_without_baselines_has_none(tmp_path: Path) -> None:
    (tmp_path / "known_gaps.json").write_text("[]\n")

    assert load_baselines(tmp_path) == []


def test_other_files_are_not_baselines(sample_baselines: Path) -> None:
    release = sample_baselines / SAMPLE_RELEASE
    (release / "http2.raw.json").write_text("{}")
    (release / "chrome_build.txt").write_text("157.0.8103.27")
    (sample_baselines / "known_gaps.json").write_text("[]\n")

    assert len(load_baselines(sample_baselines)) == 8


@pytest.mark.parametrize(
    ("text", "complaint"),
    [
        ("not json", "not JSON"),
        ("[]", "not a normal form"),
        ('{"requests": {"image": {}}}', "protocol"),
        ('{"protocol": "h2"}', "requests"),
        ('{"protocol": "h2", "requests": {}}', "requests"),
        ('{"protocol": "h2", "requests": {"prefetch": {}}}', "unknown Request Kind"),
    ],
)
def test_a_file_that_is_no_normal_form_is_refused(tmp_path: Path, text: str, complaint: str) -> None:
    release = tmp_path / "157"
    release.mkdir()
    (release / "http2.json").write_text(text)

    with pytest.raises(BaselineError, match=complaint):
        load_baselines(tmp_path)


def test_two_files_cannot_hold_the_same_baseline(sample_baselines: Path) -> None:
    release = sample_baselines / SAMPLE_RELEASE
    (release / "again.json").write_text((release / "http2.json").read_text())

    with pytest.raises(BaselineError, match=r"again\.json"):
        load_baselines(sample_baselines)


def test_the_same_baseline_in_two_releases_is_two_baselines(sample_baselines: Path) -> None:
    other = sample_baselines / "157"
    other.mkdir()
    (other / "http2.json").write_text((sample_baselines / SAMPLE_RELEASE / "http2.json").read_text())

    assert len(load_baselines(sample_baselines)) == 12


def test_a_recording_like_the_baseline_has_no_gaps(sample_baselines: Path) -> None:
    assert find_gaps(load_baselines(sample_baselines), identical_to_the_sample) == []


def test_a_gap_names_release_kind_protocol_and_field(sample_baselines: Path, change_baseline: ChangeBaseline) -> None:
    def add_header(form: dict[str, Any]) -> None:
        form["requests"]["image"]["headers"].append("x-changed: yes")

    change_baseline("http2", add_header)

    assert find_gaps(load_baselines(sample_baselines), identical_to_the_sample) == [
        Gap(SAMPLE_RELEASE, "image", "h2", "request.headers.x-changed", "x-changed: yes", None)
    ]


def test_a_connection_gap_is_one_gap_per_request_kind(sample_baselines: Path, change_baseline: ChangeBaseline) -> None:
    def change_version(form: dict[str, Any]) -> None:
        form["tls"]["legacy_version"] = "TLS 1.3"

    change_baseline("http1", change_version)

    assert find_gaps(load_baselines(sample_baselines), identical_to_the_sample) == [
        Gap(SAMPLE_RELEASE, kind.value, "http/1.1", "tls.legacy_version", "TLS 1.3", "TLS 1.2") for kind in ALL_KINDS
    ]


def test_each_baseline_is_recorded_with_its_own_chrome_release(sample_baselines: Path) -> None:
    older = sample_baselines / "155"
    older.mkdir()
    (older / "http2.json").write_text((sample_baselines / SAMPLE_RELEASE / "http2.json").read_text())
    recorded: list[str] = []

    def recorder(baseline: Baseline) -> dict[str, Json]:
        recorded.append(baseline.release)
        return baseline.fingerprint

    find_gaps(load_baselines(sample_baselines), recorder)

    assert recorded == ["155"] * 4 + [SAMPLE_RELEASE] * 8


def test_a_baseline_of_a_chrome_release_with_no_persona_cannot_be_recorded() -> None:
    baseline = Baseline("154", RequestKind.FETCH, "h2", {"protocol": "h2"})

    with pytest.raises(BaselineError, match="Chrome 154"):
        find_gaps([baseline])


def test_a_request_without_a_response_is_one_gap() -> None:
    def refused(baseline: Baseline) -> dict[str, Json]:
        raise RequestError(f"https://capture.localhost/{baseline.protocol}/{baseline.kind}", -102)

    baseline = Baseline("157", RequestKind.FETCH, "h3", {"protocol": "h3", "tls": {}})

    assert find_gaps([baseline], refused) == [Gap("157", "fetch", "h3", "response", "a response", "net error -102")]


def test_a_changed_baseline_is_a_mismatch_with_the_list_made_before_the_change(
    sample_baselines: Path, change_baseline: ChangeBaseline
) -> None:
    def change_flags(form: dict[str, Any]) -> None:
        form["requests"]["fetch"]["flags"] = ["END_HEADERS"]

    listed = find_gaps(load_baselines(sample_baselines), identical_to_the_sample)
    change_baseline("http2", change_flags)

    found = mismatches(find_gaps(load_baselines(sample_baselines), identical_to_the_sample), listed)

    assert [(gap.field, gap.baseline) for gap in found.unlisted] == [("request.flags", ["END_HEADERS"])]
