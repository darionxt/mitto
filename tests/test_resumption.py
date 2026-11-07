"""Tests of what a Session sends on a connection that resumes, through the public API. They need a native library.

A server that gives out session tickets is one the Baselines do not hold, so no Parity Check sees this. What real Chrome
sends here is in docs/capture.md, and this keeps a Session to it.
"""

import time

import pytest

import mitto
from tools.capture import (
    CROSS_SITE_HOST,
    HTTP3,
    RESUMPTION_CLOSE_PATH,
    RESUMPTION_OTHER_SITE_PATH,
    RESUMPTION_RUNS,
    CaptureServer,
    Run,
    normal_form,
)
from tools.capture.recording import Json, Recording
from tools.parity.replay import session_settings

pytestmark = pytest.mark.usefixtures("native_library", "both_apis")

_EARLY_REQUEST = "request"
_CONTROL = "control"
_QPACK_ENCODER = "qpack_encoder"
# Gives a Session time to see that a connection has ended, as the page gives Chrome.
_WAIT_SECONDS = 0.5


def go_through(session: mitto.Session, server: CaptureServer, run: Run) -> None:
    """Ask for what the pages of a resumption run ask for, in the same order, from the same pages."""

    def url(path: str, host: str | None = None) -> str:
        return server.url(run.route, host=host or run.host, path=path)

    def close_connection(initiator: str) -> None:
        session.get(url(RESUMPTION_CLOSE_PATH), kind=mitto.RequestKind.FETCH, initiator=initiator)
        time.sleep(_WAIT_SECONDS)

    fetch, post, navigation = (url(case.path) for case in run.cases)
    other_site_script = url(run.fresh_cases[0].path, CROSS_SITE_HOST)
    other_site = url(RESUMPTION_OTHER_SITE_PATH, CROSS_SITE_HOST)
    first = url(run.start_path)
    session.get(first, kind=mitto.RequestKind.NAVIGATION)
    close_connection(first)
    session.get(fetch, kind=mitto.RequestKind.FETCH, initiator=first)
    close_connection(first)
    session.post(post, kind=mitto.RequestKind.FETCH, initiator=first, data="text body")
    close_connection(first)
    session.get(navigation, kind=mitto.RequestKind.NAVIGATION, initiator=first)
    close_connection(navigation)
    session.get(other_site_script, kind=mitto.RequestKind.SCRIPT, initiator=navigation)
    time.sleep(_WAIT_SECONDS)
    session.get(other_site, kind=mitto.RequestKind.NAVIGATION, initiator=navigation)
    session.get(url(run.done_path, CROSS_SITE_HOST), kind=mitto.RequestKind.FETCH, initiator=other_site)


@pytest.fixture(params=RESUMPTION_RUNS, ids=lambda run: run.name)
def run(request: pytest.FixtureRequest) -> Run:
    return request.param


@pytest.fixture
def recording(run: Run) -> Recording:
    with (
        CaptureServer(tickets=True) as server,
        mitto.Session(_settings=session_settings(server, run)) as session,
    ):
        go_through(session, server, run)
        return server.recording(run.route, host=run.host)


def extensions(form: dict[str, Json]) -> list[Json]:
    tls = form["tls"]
    assert isinstance(tls, dict)
    found = tls["extensions"]
    assert isinstance(found, list)
    return found


def extension_names(form: dict[str, Json]) -> set[str]:
    return {str(extension["type"]) for extension in extensions(form) if isinstance(extension, dict)}


def early_data_of(form: dict[str, Json], case: str) -> Json:
    requests = form["requests"]
    assert isinstance(requests, dict)
    request = requests[case]
    assert isinstance(request, dict)
    return request.get("early_data")


def test_what_a_ticket_adds_to_a_client_hello_and_nothing_else_differs(run: Run, recording: Recording) -> None:
    fresh = normal_form(recording, run.fresh_cases)
    resumed = normal_form(recording, run.cases)

    added = {"pre_shared_key", "early_data"} if run.route == HTTP3 else {"pre_shared_key"}
    assert extension_names(resumed) - extension_names(fresh) == added
    assert extension_names(fresh) - extension_names(resumed) == set()
    assert [e for e in extensions(resumed) if isinstance(e, dict) and e["type"] not in added] == extensions(fresh)
    for part in ("protocol", "quic", "http2", "http3"):
        assert resumed.get(part) == fresh.get(part)


def test_only_what_is_safe_to_repeat_goes_as_early_data_over_http3(run: Run, recording: Recording) -> None:
    resumed = normal_form(recording, run.cases)

    if run.route == HTTP3:
        everything = [_EARLY_REQUEST, _CONTROL, _QPACK_ENCODER]
        assert early_data_of(resumed, "resumption_fetch") == everything
        assert early_data_of(resumed, "resumption_navigation") == everything
        assert early_data_of(resumed, "resumption_post") == [_CONTROL]
    else:
        assert early_data_of(resumed, "resumption_fetch") is None
        assert early_data_of(resumed, "resumption_post") is None
        assert early_data_of(resumed, "resumption_navigation") is None


def test_a_ticket_stays_with_the_top_level_site_that_got_it(run: Run, recording: Recording) -> None:
    """The script at the other site got a ticket for its name, which is of no use to a navigation to that name."""
    other_site = [
        connection
        for connection in recording.connections
        if any(request.target == RESUMPTION_OTHER_SITE_PATH for request in connection.requests)
    ]

    assert [connection.resumed for connection in other_site] == [False]
