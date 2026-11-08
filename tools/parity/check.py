"""One Parity Check: load Baselines, make the same requests through the public API, and find the gaps."""

import json
import tempfile
from collections.abc import Callable, Iterable
from dataclasses import dataclass, replace
from pathlib import Path
from typing import cast

from mitto import RequestError, Session
from mitto._persona import WINDOWS_PERSONAS
from tools.capture import CaptureServer, Case, Recording, normal_form
from tools.capture.recording import Json
from tools.parity.gaps import Gap, differences
from tools.parity.replay import needs_cache, proxy_of, replay, run_of, session_settings

BASELINES_DIRECTORY = Path(__file__).parents[2] / "tests" / "baselines"

_RAW_SUFFIX = ".raw.json"
_REQUESTS = "requests"
_REQUEST = "request"
_RESPONSE_FIELD = "response"
_A_RESPONSE = "a response"

_REFUSED: Json = {"refused": True}
"""A Baseline's request when Chrome refused to send it at all."""

_REFUSAL_ERROR = "ERR_ALPN_NEGOTIATION_FAILED"
"""What a client sees when //net settles on HTTP/1.1 for a stream body, which it refuses to send over that."""

type Recorder = Callable[[Baseline], dict[str, Json]]
"""What makes the request of a Baseline, and returns the fingerprint of what it sent."""


class BaselineError(Exception):
    """A file under the Baselines directory is not a usable Baseline."""


@dataclass(frozen=True, slots=True)
class Baseline:
    """What real Chrome sent for one Chrome Release, Recording Case and protocol.

    Attributes:
        release: The Chrome Release, such as "157".
        case: The name of the Recording Case, such as "navigation" or "fetch_post_text_cross_site".
        protocol: The protocol, such as "h2".
        fingerprint: See fingerprint().
    """

    release: str
    case: str
    protocol: str
    fingerprint: dict[str, Json]


def fingerprint(form: dict[str, Json], case: str) -> dict[str, Json]:
    """Return the part of a normal form that is about one Recording Case.

    That is everything about its connection, and that case's entry in "requests" as "request". The entry is None if the
    normal form has none.
    """
    connection = {key: value for key, value in form.items() if key != _REQUESTS}
    requests = form.get(_REQUESTS)
    return {**connection, _REQUEST: requests.get(case) if isinstance(requests, dict) else None}


def load_baselines(directory: Path) -> list[Baseline]:
    """Read the Baselines from directory, in a fixed order.

    Each file named RELEASE/NAME.json or RELEASE/cases/NAME.json holds a normal form, with one Baseline for each Recording
    Case in it. Other files, such as raw recordings and notes on where a Baseline came from, are left alone.

    Raises:
        BaselineError: A file is not a normal form, or two files hold the same Baseline.
    """
    baselines: list[Baseline] = []
    seen: dict[tuple[str, str, str], Path] = {}
    for path in sorted((*directory.glob("*/*.json"), *directory.glob("*/cases/*.json"))):
        if path.name.endswith(_RAW_SUFFIX):
            continue
        for baseline in _read_baselines(path, path.relative_to(directory).parts[0]):
            key = (baseline.release, baseline.case, baseline.protocol)
            if key in seen:
                raise BaselineError(f"{path} and {seen[key]} both hold {key}")
            seen[key] = path
            baselines.append(baseline)
    return baselines


def _read_baselines(path: Path, release: str) -> list[Baseline]:
    try:
        form = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        raise BaselineError(f"{path} is not JSON: {error}") from error
    if not isinstance(form, dict):
        raise BaselineError(f"{path} is not a normal form")
    form = cast(dict[str, Json], form)
    protocol = form.get("protocol")
    requests = form.get(_REQUESTS)
    if not isinstance(protocol, str) or not isinstance(requests, dict) or not requests:
        raise BaselineError(f"{path} is not a normal form with a protocol and requests")
    for case in requests:
        if run_of(case, protocol) is None:
            raise BaselineError(f"{path} holds an unknown Recording Case: {case} over {protocol}")
    return [Baseline(release, case, protocol, fingerprint(form, case)) for case in requests]


def _persona_of(release: str) -> str:
    name = WINDOWS_PERSONAS.get(release)
    if name is None:
        raise BaselineError(f"no Persona presents Chrome {release}, so its Baselines cannot be checked")
    return name


def record(baseline: Baseline) -> dict[str, Json]:
    """Replay the Recording Case of a Baseline through a new Session to a new capture server.

    The request presents the Persona of the Baseline's Chrome Release, with a Machine drawn for the Session as a caller's
    is, whose values the normal form hides. It goes where the Chrome that recorded the Baseline went, also through the
    server's proxy. The Session accepts the server's private certificate and finds the case pages' names, in settings that
    the public API keeps private. Over HTTP/3 it has Chrome's --origin-to-force-quic-on as well, because //net refuses QUIC
    to that certificate otherwise.

    Returns:
        The fingerprint of what the capture server received. A request that never reached the server is None there, and
        with no connection to compare, its connection counts as the Baseline's, so that it is one gap.

    Raises:
        BaselineError: No Persona presents the Baseline's Chrome Release, or no run records its Recording Case.
        RequestError: A request got no response, and that is not what the Baseline shows.
        RecordingError: The connections that carried the requests do not look the same.
        NativeLibraryError: The native library cannot be used.
    """
    persona = _persona_of(baseline.release)
    run = run_of(baseline.case, baseline.protocol)
    if run is None:
        raise BaselineError(f"no run records {baseline.case} over {baseline.protocol}")
    refused: Json = None
    with (
        CaptureServer() as server,
        tempfile.TemporaryDirectory(prefix="mitto-parity-") as cache_directory,
        Session(
            persona=persona,
            proxy=proxy_of(server, run),
            cache=cache_directory if needs_cache(baseline.case) else None,
            _settings=session_settings(server, run),
        ) as session,
    ):
        try:
            replay(session, server, run, baseline.case)
        except RequestError as error:
            if error.name != _REFUSAL_ERROR or baseline.fingerprint.get(_REQUEST) != _REFUSED:
                raise
            refused = _REFUSED
    recording = server.recording(run.route, host=run.host)
    cases = [as_recorded(case, recording) for case in run.cases]
    if not any(_was_recorded(case, recording) for case in cases):
        return {**baseline.fingerprint, _REQUEST: refused}
    return fingerprint(normal_form(recording, cases), baseline.case)


def _was_recorded(case: Case, recording: Recording) -> bool:
    return any(case.matches(request) for connection in recording.connections for request in connection.requests)


def as_recorded(case: Case, recording: Recording) -> Case:
    """Return a case, or if no request carries what it should, the case that any request to its path stands for.

    So a request without its Cookie is a gap in that header, rather than a request that is missing.
    """
    return case if case.carries is None or _was_recorded(case, recording) else replace(case, carries=None)


def find_gaps(baselines: Iterable[Baseline], recorder: Recorder = record) -> list[Gap]:
    """Make the request of each Baseline, and return every gap in the order of the Baselines and of their fields.

    A request that gets no response has nothing to compare, so it is one gap, in the field "response".

    Raises:
        BaselineError: No Persona presents the Chrome Release of a Baseline, or no run records its Recording Case.
        RecordingError: The connections that carried a Baseline's requests do not look the same.
        NativeLibraryError: The native library cannot be used.
    """
    gaps: list[Gap] = []
    for baseline in baselines:
        try:
            recorded = recorder(baseline)
        except RequestError as error:
            found = [(_RESPONSE_FIELD, _A_RESPONSE, f"net error {error.net_error}")]
        else:
            found = differences(baseline.fingerprint, recorded)
        gaps += [Gap(baseline.release, baseline.case, baseline.protocol, *difference) for difference in found]
    return gaps
