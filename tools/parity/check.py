"""One Parity Check: load Baselines, make the same requests through the native core, and find the gaps."""

import json
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from mitto._native import RequestError, Session
from tools.capture import KIND_PATHS, CaptureServer, RequestKind, normal_form
from tools.capture.recording import Json
from tools.parity.gaps import Gap, differences

BASELINES_DIRECTORY = Path(__file__).parents[2] / "tests" / "baselines"

_RAW_SUFFIX = ".raw.json"
_REQUESTS = "requests"
_RESPONSE_FIELD = "response"
_A_RESPONSE = "a response"

type Recorder = Callable[[str, RequestKind], dict[str, Json]]
"""What makes a request of a Request Kind over a protocol, and returns the fingerprint of what it sent."""


class BaselineError(Exception):
    """A file under the Baselines directory is not a usable Baseline."""


@dataclass(frozen=True, slots=True)
class Baseline:
    """What real Chrome sent for one Chrome Release, Request Kind and protocol.

    Attributes:
        release: The Chrome Release, such as "157".
        kind: The Request Kind.
        protocol: The protocol, such as "h2".
        fingerprint: See fingerprint().
    """

    release: str
    kind: RequestKind
    protocol: str
    fingerprint: dict[str, Json]


def fingerprint(form: dict[str, Json], kind: RequestKind) -> dict[str, Json]:
    """Return the part of a normal form that is about one Request Kind.

    That is everything about its connection, and that kind's entry in "requests" as "request". The entry is None if the
    normal form has none.
    """
    connection = {key: value for key, value in form.items() if key != _REQUESTS}
    requests = form.get(_REQUESTS)
    return {**connection, "request": requests.get(kind.value) if isinstance(requests, dict) else None}


def load_baselines(directory: Path) -> list[Baseline]:
    """Read the Baselines from directory, in a fixed order.

    Each file named RELEASE/NAME.json holds a normal form, with one Baseline for each Request Kind in it. Other files,
    such as raw recordings and notes on where a Baseline came from, are left alone.

    Raises:
        BaselineError: A file is not a normal form, or two files hold the same Baseline.
    """
    baselines: list[Baseline] = []
    seen: dict[tuple[str, RequestKind, str], Path] = {}
    for path in sorted(directory.glob("*/*.json")):
        if path.name.endswith(_RAW_SUFFIX):
            continue
        for baseline in _read_baselines(path):
            key = (baseline.release, baseline.kind, baseline.protocol)
            if key in seen:
                raise BaselineError(f"{path} and {seen[key]} both hold {key}")
            seen[key] = path
            baselines.append(baseline)
    return baselines


def _read_baselines(path: Path) -> list[Baseline]:
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
    try:
        kinds = [RequestKind(name) for name in requests]
    except ValueError as error:
        raise BaselineError(f"{path} holds an unknown Request Kind: {error}") from error
    return [Baseline(path.parent.name, kind, protocol, fingerprint(form, kind)) for kind in kinds]


def record(protocol: str, kind: RequestKind) -> dict[str, Json]:
    """Make one request of a Request Kind through a new native Session to a new capture server.

    Persona data does not exist yet, so the request holds only what //net adds by itself.

    Returns:
        The fingerprint of what the capture server received.

    Raises:
        RequestError: The request got no response.
        NativeLibraryError: The native library cannot be used.
    """
    with CaptureServer() as server, Session(ignore_certificate_errors_spki_list=[server.spki_hash]) as session:
        session.request(f"https://{server.authority(protocol)}{KIND_PATHS[kind]}")
    return fingerprint(normal_form(server.recording(protocol)), kind)


def find_gaps(baselines: Iterable[Baseline], recorder: Recorder = record) -> list[Gap]:
    """Make the request of each Baseline, and return every gap in the order of the Baselines and of their fields.

    A request that gets no response has nothing to compare, so it is one gap, in the field "response".

    Raises:
        NativeLibraryError: The native library cannot be used.
    """
    gaps: list[Gap] = []
    for baseline in baselines:
        try:
            recorded = recorder(baseline.protocol, baseline.kind)
        except RequestError as error:
            found = [(_RESPONSE_FIELD, _A_RESPONSE, f"net error {error.net_error}")]
        else:
            found = differences(baseline.fingerprint, recorded)
        gaps += [Gap(baseline.release, baseline.kind.value, baseline.protocol, *difference) for difference in found]
    return gaps
