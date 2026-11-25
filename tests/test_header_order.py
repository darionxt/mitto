"""Tests of the order on the wire of the header lines that a page's script sets on fetch() and XMLHttpRequest, and that a
caller gives a request of their kinds.

tests/header_order_cases.js makes the requests in a page, and tests/data/header_order_cases.json holds the names of the
lines that Chrome 157 sent for each, in their order, over HTTP/1.1, HTTP/2 and HTTP/3: headless Chrome for Testing on macOS
(157.0.8103.79), on the capture server's case pages. Tests that take the native_library fixture are skipped when this
machine has no native library.
"""

from collections.abc import Iterator
from typing import Any

import pytest

from mitto import Machine, RequestKind, Session, _native
from mitto._persona import CHROME_157_WINDOWS
from tests.header_order import AS_CHROME, PAGE_PATH, SETS, answer, cases, names_of
from tools.capture import HTTP1, HTTP2, HTTP3, CaptureServer, Run
from tools.capture import server as server_module
from tools.parity.replay import run_of, session_settings

_TIMEOUT_SECONDS = 60
_OTHER_KINDS = [kind for kind in RequestKind if kind not in {RequestKind.FETCH, RequestKind.XHR}]
# What //net adds after a request's own lines.
_NET_LINES = {"referer", "accept-encoding", "accept-language"}


@pytest.fixture
def server(native_library: Any, monkeypatch: pytest.MonkeyPatch) -> Iterator[CaptureServer]:
    monkeypatch.setattr(server_module, "respond_on", answer)
    with CaptureServer() as server:
        yield server


def _run(protocol: str) -> Run:
    run = run_of("fetch_post_text_same_origin", protocol)
    assert run is not None
    return run


def _session(server: CaptureServer, run: Run, machine: Machine | None = None) -> Session:
    settings = session_settings(server, run)
    if machine is None:
        return Session(persona=CHROME_157_WINDOWS, _settings=settings)
    return Session(persona=CHROME_157_WINDOWS, machine=machine, _settings=settings)


def _ask(session: Session, server: CaptureServer, run: Run, case: str, kind: RequestKind, **options: Any) -> list[str]:
    """Make a request from the page, and return the names of the lines that the server saw of it, in their order."""
    initiator = server.url(run.route, host=run.host, path=PAGE_PATH)
    url = server.url(run.route, host=run.host, path=f"/order/{case}")
    method = options.pop("method", "GET")
    session.request(method, url, kind=kind, initiator=initiator, **options)
    return names_of(server, run)[case]


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_pages_fetches_send_a_scripts_lines_where_chrome_does(server: CaptureServer, protocol: str) -> None:
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    run = _run(protocol)
    page_url = server.url(run.route, host=run.host, path=PAGE_PATH)

    with _session(server, run) as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(cases(("fetch",)), timeout=_TIMEOUT_SECONDS) == "done"
        names = names_of(server, run)

    expected = {case: lines for case, lines in AS_CHROME[protocol].items() if "-fetch-" in case}
    assert set(names) == set(expected)
    for case, lines in expected.items():
        assert names[case] == lines, case


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.XHR])
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_callers_fetches_and_xhrs_send_its_lines_where_chrome_does(
    server: CaptureServer, protocol: str, kind: RequestKind
) -> None:
    run = _run(protocol)

    with _session(server, run) as session:
        for name, headers in SETS.items():
            for verb, options in {
                "get": {"headers": headers},
                "post": {"method": "POST", "data": "{}", "headers": headers},
                "put": {"method": "PUT", "data": "{}", "headers": {**headers, "Content-Type": "application/json"}},
            }.items():
                case = f"window-{kind.value}-{verb}-{name}"
                lines = _ask(session, server, run, case, kind, **options)
                # A line that the Persona has keeps the Persona's spelling of its name.
                assert [line.lower() for line in lines] == [line.lower() for line in AS_CHROME[protocol][case]], case


def test_the_order_depends_on_the_names_alone(server: CaptureServer) -> None:
    run = _run(HTTP1)
    expected = AS_CHROME[HTTP1]["window-fetch-get-many"]

    for seed in (1, 2):
        with _session(server, run, Machine.draw(seed=seed)) as session:
            headers = {name: f"{seed} {name}" for name in reversed(SETS["many"])}
            lines = _ask(session, server, run, f"many-{seed}", RequestKind.FETCH, headers=headers)
        assert [line.lower() for line in lines] == [line.lower() for line in expected]


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.XHR])
def test_a_line_that_no_script_sets_keeps_its_place(server: CaptureServer, kind: RequestKind) -> None:
    run = _run(HTTP1)

    with _session(server, run) as session:
        plain = _ask(session, server, run, "plain", kind)
        removed = _ask(session, server, run, "removed", kind, headers={"Accept": None, "sec-ch-ua": None})
        forbidden = _ask(
            session, server, run, "forbidden", kind, headers={"Accept-Encoding": "br", "User-Agent": "mine", "DNT": "1"}
        )
        mixed = _ask(session, server, run, "mixed", kind, headers={"DNT": "1", "X-Alpha": "1"})

    assert removed == [name for name in plain if name not in {"Accept", "sec-ch-ua"}]
    net = next(index for index, name in enumerate(plain) if name.lower() in _NET_LINES)
    rest = [name for name in plain[net:] if name != "Accept-Encoding"]
    assert forbidden == [*plain[:net], "Accept-Encoding", "DNT", *rest]
    one = AS_CHROME[HTTP1][f"window-{kind.value}-get-one"]
    at = next(index for index, name in enumerate(one) if name.lower() in _NET_LINES)
    assert mixed == [*one[:at], "DNT", *one[at:]]


@pytest.mark.parametrize("kind", _OTHER_KINDS)
def test_a_callers_lines_on_another_kind_follow_the_personas(server: CaptureServer, kind: RequestKind) -> None:
    run = _run(HTTP1)
    body: dict[str, Any] = {"method": "POST", "data": "text"} if kind is RequestKind.BEACON else {}

    with _session(server, run) as session:
        plain = _ask(session, server, run, "plain", kind, **body)
        lines = _ask(
            session, server, run, "lines", kind, headers={"X-Gamma": "3", "Accept": "a/b", "X-Alpha": "1"}, **body
        )

    net = next(index for index, name in enumerate(plain) if name.lower() in _NET_LINES)
    assert lines == [*plain[:net], "X-Gamma", "X-Alpha", *plain[net:]]


def test_two_names_that_differ_only_in_case_are_refused(server: CaptureServer) -> None:
    run = _run(HTTP1)

    with _session(server, run) as session, pytest.raises(ValueError, match="differ only in case"):
        _ask(session, server, run, "twice", RequestKind.FETCH, headers={"X-One": "1", "x-one": "2"})
