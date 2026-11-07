"""Tests of replaying the Recording Cases. Tests that take the native_library fixture need a native library."""

from dataclasses import replace
from pathlib import Path
from types import TracebackType
from typing import Any, Self

import pytest

from mitto import RequestError, Session
from tools.capture import CASES, HTTP1, HTTP2, HTTP3, RUNS, CaptureServer, Case, Run, normal_form
from tools.capture.cases import PROXY_CONNECT
from tools.capture.http1 import Http1Request
from tools.capture.recording import ConnectionRecord, Json, Recording
from tools.parity import (
    BASELINES_DIRECTORY,
    KNOWN_GAPS_FILE_NAME,
    Baseline,
    Gap,
    find_gaps,
    load_baselines,
    loads_known_gaps,
    mismatches,
)
from tools.parity.check import as_recorded, record
from tools.parity.replay import REPLAYS, protocol_of, proxy_of, replay, run_of, session_settings

_RELEASE = "157"
_REFUSED: dict[str, Json] = {"refused": True}
_REFUSAL = "ERR_ALPN_NEGOTIATION_FAILED"
_CANCEL: Json = {"type": "RST_STREAM", "flags": [], "payload": "00000008"}
_WHERE_A_REQUEST_WENT = {":method", ":authority", ":scheme", ":path", "host"}


def _listed_gaps() -> list[Gap]:
    return loads_known_gaps((BASELINES_DIRECTORY / KNOWN_GAPS_FILE_NAME).read_text(encoding="utf-8"))


def _recording_of(*requests: Http1Request) -> Recording:
    connection = ConnectionRecord(1, None, None, None, None, (), requests, None)
    return Recording("capture.test", "capture.test:1", (connection,))


def _request(target: str, *headers: tuple[str, str]) -> Http1Request:
    return Http1Request("GET", target, "HTTP/1.1", headers)


class _FailingSession:
    """Stands in for a Session whose every request fails with one net error, and reaches no server."""

    error_name = _REFUSAL

    def __init__(self, **_settings: Any) -> None:
        pass

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        pass

    def request(self, method: str, url: str, **_options: Any) -> None:
        raise RequestError(url, -1, method, self.error_name)


def test_every_recording_case_has_a_replay() -> None:
    assert set(REPLAYS) == {case.name for case in CASES}


@pytest.mark.parametrize("run", RUNS, ids=lambda run: run.name)
def test_each_case_of_a_run_finds_the_run_by_its_name_and_protocol(run: Run) -> None:
    for case in run.cases:
        assert run_of(case.name, protocol_of(run.route)) is run


def test_no_run_records_a_case_over_a_protocol_it_does_not_have() -> None:
    assert run_of("plain_http_fetch", HTTP2) is None
    assert run_of("proxy_navigation", HTTP3) is None
    assert run_of("no_such_case", HTTP1) is None


def test_only_a_later_hop_and_what_opens_a_tunnel_ask_for_another_case() -> None:
    starts = {name: replay.start for name, replay in REPLAYS.items() if replay.start is not None}

    assert starts == {
        **{f"redirect_{kind}_{hop}": f"redirect_{kind}_1" for kind in ("navigation", "fetch") for hop in range(1, 5)},
        "proxy_connect": "proxy_navigation",
    }


def test_a_case_that_carries_a_header_counts_the_request_that_has_it() -> None:
    case = Case("cookie", "/case/cookie", "cookie")
    recording = _recording_of(_request("/case/cookie"), _request("/case/cookie", ("Cookie", "mitto=1")))

    assert as_recorded(case, recording) is case


def test_a_case_that_carries_a_header_counts_any_request_to_its_path_when_none_has_it() -> None:
    case = Case("cookie", "/case/cookie", "cookie")

    found = as_recorded(case, _recording_of(_request("/case/cookie")))

    assert found.carries is None
    assert found.matches(_request("/case/cookie"))


def test_a_case_that_carries_nothing_stays_the_same_case() -> None:
    # The normal form finds what opened a tunnel by this very object.
    assert as_recorded(PROXY_CONNECT, _recording_of(_request("/case/proxy-navigation"))) is PROXY_CONNECT


@pytest.mark.parametrize("run", RUNS, ids=lambda run: run.name)
def test_a_replay_goes_through_a_proxy_and_is_told_how_to_find_names_as_the_chrome_of_its_run_was(run: Run) -> None:
    with CaptureServer() as server:
        flags = server.chrome_flags(run)
        proxy = proxy_of(server, run)
        rules = session_settings(server, run).get("host_resolver_rules")

    assert (f"--proxy-server={proxy}" in flags) == (proxy is not None)
    assert (f"--host-resolver-rules={rules}" in flags) == bool(rules)


def test_a_refusal_with_nothing_on_the_wire_matches_a_baseline_of_a_refusal(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("tools.parity.check.Session", _FailingSession)
    baseline = Baseline(_RELEASE, "fetch_post_stream", HTTP1, {"protocol": HTTP1, "tls": {}, "request": _REFUSED})

    assert find_gaps([baseline]) == []


def test_a_refusal_is_a_gap_when_the_baseline_has_a_request(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("tools.parity.check.Session", _FailingSession)
    baseline = Baseline(_RELEASE, "fetch_post_stream", HTTP2, {"protocol": HTTP2, "request": {"headers": []}})

    assert find_gaps([baseline]) == [
        Gap(_RELEASE, "fetch_post_stream", HTTP2, "response", "a response", "net error -1")
    ]


def test_another_error_is_a_gap_when_the_baseline_has_a_refusal(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("tools.parity.check.Session", _FailingSession)
    monkeypatch.setattr(_FailingSession, "error_name", "ERR_CONNECTION_REFUSED")
    baseline = Baseline(_RELEASE, "fetch_post_stream", HTTP1, {"protocol": HTTP1, "request": _REFUSED})

    assert find_gaps([baseline]) == [
        Gap(_RELEASE, "fetch_post_stream", HTTP1, "response", "a response", "net error -1")
    ]


@pytest.mark.usefixtures("native_library")
def test_a_stream_is_refused_over_http1_as_the_baseline_shows() -> None:
    baseline = Baseline(_RELEASE, "fetch_post_stream", HTTP1, {"protocol": HTTP1, "tls": {}, "request": _REFUSED})

    assert record(baseline)["request"] == _REFUSED
    assert find_gaps([baseline]) == []


@pytest.mark.usefixtures("native_library")
def test_a_stream_that_goes_over_http1_is_not_taken_for_a_refusal(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(REPLAYS, "fetch_post_stream", replace(REPLAYS["fetch_post_stream"], body={"data": b"bytes"}))
    baseline = Baseline(_RELEASE, "fetch_post_stream", HTTP1, {"protocol": HTTP1, "tls": {}, "request": _REFUSED})

    assert record(baseline)["request"] != _REFUSED


def test_no_known_gap_says_that_a_replay_went_somewhere_else() -> None:
    """A gap in these lines would mean the replay is wrong, and that what it shows is not about the native core."""
    for gap in _listed_gaps():
        assert gap.field not in {"protocol", "response"}, gap
        name = gap.field.removeprefix("request.headers.")
        assert name.lower() not in _WHERE_A_REQUEST_WENT, gap
        assert gap.field != "request.request_line" or gap.baseline is None, gap


@pytest.mark.usefixtures("native_library")
def test_a_cache_in_a_directory_changes_nothing_that_any_case_sends(monkeypatch: pytest.MonkeyPatch) -> None:
    """Every Chrome behind the Baselines had one, and a Session by default has none, so both have to compare alike."""

    def needs_cache(_case: str) -> bool:
        return True

    monkeypatch.setattr("tools.parity.check.needs_cache", needs_cache)
    baselines = [baseline for baseline in load_baselines(BASELINES_DIRECTORY) if baseline.release == _RELEASE]

    assert not mismatches(find_gaps(baselines), [gap for gap in _listed_gaps() if gap.release == _RELEASE])


def _frames_after_a_revalidation(cache: Path | str) -> Json:
    run = run_of("revalidation", HTTP2)
    assert run is not None
    with CaptureServer() as server, Session(cache=cache, _settings=session_settings(server, run)) as session:
        replay(session, server, run, "revalidation")
    requests = normal_form(server.recording(run.route, host=run.host), run.cases)["requests"]
    assert isinstance(requests, dict)
    request = requests["revalidation"]
    assert isinstance(request, dict)
    return request.get("later_frames")


@pytest.mark.usefixtures("native_library")
def test_a_cache_in_a_directory_lets_the_stream_of_a_304_end(tmp_path: Path) -> None:
    for attempt in range(5):
        assert _frames_after_a_revalidation(tmp_path / str(attempt)) is None


@pytest.mark.usefixtures("native_library")
def test_a_cache_in_memory_cancels_the_stream_of_a_304() -> None:
    """docs/parity.md says so. A response can get there first, which ends the stream with nothing to cancel."""
    assert any(_frames_after_a_revalidation("memory") == [_CANCEL] for _ in range(5))


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("run", RUNS, ids=lambda run: run.name)
def test_a_replay_goes_to_the_path_of_its_case_and_shows_no_gaps_against_itself(run: Run) -> None:
    protocol = protocol_of(run.route)
    baselines: list[Baseline] = []
    for case in run.cases:
        if case.refused_path is not None and protocol == HTTP1:
            continue
        recorded = record(Baseline(_RELEASE, case.name, protocol, {}))
        assert recorded["protocol"] == protocol
        # What opens a tunnel has no path, because it names a server instead.
        where = "CONNECT <authority>" if case is PROXY_CONNECT else case.path
        assert where in str(recorded["request"]), case.name
        baselines.append(Baseline(_RELEASE, case.name, protocol, recorded))

    assert not mismatches(find_gaps(baselines), [])
