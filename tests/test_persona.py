"""Tests of Persona data against the Baselines of its Chrome Release.

They need no native library and no connection, so they run on every machine. What //net adds is written out here, and
the Parity Checks show that //net does add it.
"""

import itertools
import re

import pytest

from mitto._native import FetchParams, Priority
from mitto._persona import WINDOWS_PERSONAS, RequestDefaults, RequestKind, load_persona
from mitto._request import build_request
from tests.client_hints import HINTS_A_SERVER_ASKS_FOR, SENT_BY_DEFAULT, until_a_server_asks
from tools.capture import HTTP1, HTTP2, HTTP3, KIND_PATHS
from tools.capture import RequestKind as CaptureRequestKind
from tools.capture.normal_form import (
    AUTHORITY_PLACEHOLDER,
    HINTS_FROM_THE_MACHINE,
    MACHINE_PLACEHOLDER,
    ORIGIN_PLACEHOLDER,
)
from tools.parity import BASELINES_DIRECTORY, Baseline, load_baselines
from tools.parity.replay import REPLAYS

PERSONAS = {release: load_persona(name) for release, name in WINDOWS_PERSONAS.items()}
ALL_BASELINES = load_baselines(BASELINES_DIRECTORY)
BASELINES = [baseline for baseline in ALL_BASELINES if baseline.case in {kind.value for kind in RequestKind}]

_CLIENT_HINTS_PREFIX = "client_hints_"

# A Baseline of a refusal holds no headers, and one that has hints depends on what an origin asked for.
BODY_BASELINES = [
    baseline
    for baseline in ALL_BASELINES
    if REPLAYS[baseline.case].body
    and baseline.fingerprint["request"] != {"refused": True}
    and not baseline.case.startswith(_CLIENT_HINTS_PREFIX)
]

_ANY_URL = "https://example.test:8444/any/path?query"

# What //net adds, and what says where a request went.
_NOT_FROM_THE_PERSONA = {
    "host",
    "connection",
    "content-length",
    "referer",
    "accept-encoding",
    "accept-language",
    "priority",
}

# The native core gives this line its value, from the Initiator, and the Parity Checks show that value. The line still has
# to be in its place here.
_VALUE_FROM_THE_NATIVE_CORE = "sec-fetch-site"

# What the native core leaves of the Persona's lines, for a GET that a kind makes without a body or an Initiator.
_NOT_ON_A_GET = "Origin"

# What a Persona claims for each hint that follows the machine, which a Baseline does not hold.
_CLAIMED_FROM_THE_MACHINE = {
    "device-memory": "8",
    "downlink": "10",
    "dpr": "1",
    "ect": "4g",
    "rtt": "50",
    "sec-ch-device-memory": "8",
    "sec-ch-dpr": "1",
    "sec-ch-prefers-color-scheme": "light",
    "sec-ch-prefers-reduced-motion": "no-preference",
    "sec-ch-prefers-reduced-transparency": "no-preference",
    "sec-ch-viewport-height": "945",
    "sec-ch-viewport-width": "1920",
    "viewport-width": "1920",
}

# The boundary as a normal form writes it.
_RANDOM_BOUNDARY = re.compile(r"(----WebKitFormBoundary)[0-9A-Za-z]{16}")

# The default urgency, which //net leaves out of the priority header.
_DEFAULT_URGENCY = 3

# What //net makes of a request's priority: the urgency for the priority header, and the HTTP/2 stream weight.
_URGENCY_AND_WEIGHT = {Priority.HIGHEST: (0, 256), Priority.MEDIUM: (1, 220), Priority.LOWEST: (3, 147)}


def _defaults(baseline: Baseline) -> RequestDefaults:
    return PERSONAS[baseline.release].request_defaults(RequestKind(baseline.case))


def _priority_field(defaults: RequestDefaults) -> str:
    urgency, _ = _URGENCY_AND_WEIGHT[defaults["priority"]]
    parts = [f"u={urgency}"] if urgency != _DEFAULT_URGENCY else []
    return ", ".join([*parts, "i"] if defaults["priority_incremental"] else parts)


def _lines_on_the_wire(baseline: Baseline) -> list[str]:
    """Return the header lines that the Persona's data and //net together give for a Baseline's request."""
    defaults = _defaults(baseline)
    lines = [f"{name}: {value}" for name, value in until_a_server_asks(defaults["headers"]) if name != _NOT_ON_A_GET]
    if baseline.case != RequestKind.NAVIGATION:
        lines.append(f"Referer: {ORIGIN_PLACEHOLDER}/")
    accept_language = PERSONAS[baseline.release].accept_language
    lines += ["Accept-Encoding: gzip, deflate, br, zstd", f"Accept-Language: {accept_language}"]
    if baseline.protocol == HTTP1:
        return [f"Host: {AUTHORITY_PLACEHOLDER}", "Connection: keep-alive", *lines]
    lower_case = [f"{name.lower()}:{value}" for name, _, value in (line.partition(":") for line in lines)]
    return [
        ":method: GET",
        f":authority: {AUTHORITY_PLACEHOLDER}",
        ":scheme: https",
        f":path: {KIND_PATHS[CaptureRequestKind(baseline.case)]}",
        *lower_case,
        f"priority: {_priority_field(defaults)}",
    ]


def _name(baseline: Baseline) -> str:
    return f"{baseline.release}-{baseline.case}-{baseline.protocol}"


def test_the_personas_and_the_baselines_cover_the_same_chrome_releases_request_kinds_and_protocols() -> None:
    found = {(baseline.release, baseline.case, baseline.protocol) for baseline in BASELINES}

    assert found == set(
        itertools.product(WINDOWS_PERSONAS, (kind.value for kind in RequestKind), (HTTP1, HTTP2, HTTP3))
    )


@pytest.mark.parametrize("baseline", BASELINES, ids=_name)
def test_the_headers_are_those_of_the_baseline_in_its_order(baseline: Baseline) -> None:
    request = baseline.fingerprint["request"]

    assert isinstance(request, dict)
    assert request["headers"] == _lines_on_the_wire(baseline)


@pytest.mark.parametrize("baseline", [item for item in BASELINES if item.protocol == HTTP2], ids=_name)
def test_the_priority_gives_the_http2_weight_of_the_baseline(baseline: Baseline) -> None:
    request = baseline.fingerprint["request"]
    _, weight = _URGENCY_AND_WEIGHT[_defaults(baseline)["priority"]]

    assert isinstance(request, dict)
    assert request["priority"] == {"exclusive": True, "weight": weight}


@pytest.mark.parametrize("baseline", [item for item in BASELINES if item.protocol == HTTP3], ids=_name)
def test_the_priority_gives_the_http3_priority_update_of_the_baseline(baseline: Baseline) -> None:
    request = baseline.fingerprint["request"]

    assert isinstance(request, dict)
    assert request["priority_updates"] == [_priority_field(_defaults(baseline))]


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
@pytest.mark.parametrize(
    ("kind", "fetch"),
    [
        (RequestKind.NAVIGATION, FetchParams("navigate", "document", "include", has_user_activation=True)),
        (RequestKind.FETCH, FetchParams("cors", "empty", "same-origin")),
        (RequestKind.SCRIPT, FetchParams("no-cors", "script", "include")),
        (RequestKind.IMAGE, FetchParams("no-cors", "image", "include")),
    ],
)
def test_a_kind_says_what_chrome_says_of_such_a_request(release: str, kind: RequestKind, fetch: FetchParams) -> None:
    assert PERSONAS[release].request_defaults(kind)["fetch"] == fetch


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_the_persona_says_which_chrome_it_presents(release: str) -> None:
    assert (PERSONAS[release].release, PERSONAS[release].operating_system) == (release, "Windows")


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
@pytest.mark.parametrize("kind", RequestKind)
def test_the_user_agent_is_the_one_of_every_request_kind(release: str, kind: RequestKind) -> None:
    headers = dict(PERSONAS[release].request_defaults(kind)["headers"])

    assert headers["User-Agent"] == PERSONAS[release].user_agent


@pytest.mark.parametrize("baseline", [item for item in ALL_BASELINES if item.case == "proxy_connect"], ids=_name)
def test_the_user_agent_is_the_one_that_chrome_sends_in_a_connect(baseline: Baseline) -> None:
    request = baseline.fingerprint["request"]

    assert isinstance(request, dict)
    assert request["headers"] == [
        f"Host: {AUTHORITY_PLACEHOLDER}",
        "Proxy-Connection: keep-alive",
        f"User-Agent: {PERSONAS[baseline.release].user_agent}",
    ]


def test_the_request_kinds_are_those_of_the_capture_kit() -> None:
    assert [kind.value for kind in RequestKind] == [kind.value for kind in CaptureRequestKind]


def _from_the_persona(lines: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Return the lines that a Persona decides, by lower-case name, with the value of what the native core sets left out."""
    found: list[tuple[str, str]] = []
    for name, value in lines:
        lower_name = name.lower()
        if lower_name in _NOT_FROM_THE_PERSONA or lower_name.startswith(":"):
            continue
        found.append((lower_name, "" if lower_name == _VALUE_FROM_THE_NATIVE_CORE else value))
    return found


@pytest.mark.parametrize("baseline", BODY_BASELINES, ids=_name)
def test_a_body_adds_the_lines_of_the_baseline_in_its_order(baseline: Baseline) -> None:
    replay = REPLAYS[baseline.case]
    built = build_request(PERSONAS[baseline.release], "POST", _ANY_URL, kind=replay.kind.value, **replay.body)
    request = baseline.fingerprint["request"]
    assert isinstance(request, dict)
    recorded = request["headers"]
    assert isinstance(recorded, list)

    sent = [(name, _RANDOM_BOUNDARY.sub(r"\1<random>", value)) for name, value in until_a_server_asks(built["headers"])]

    assert _from_the_persona(sent) == _from_the_persona(
        [(name, value) for name, _, value in (str(line).partition(": ") for line in recorded)]
    )


def _as_recorded(lines: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Return what a Persona decides of these lines, as a Baseline holds it, with the machine's values hidden."""
    return [
        (name, MACHINE_PLACEHOLDER if name in HINTS_FROM_THE_MACHINE else value)
        for name, value in _from_the_persona(lines)
    ]


def _recorded(baseline: Baseline) -> list[tuple[str, str]]:
    request = baseline.fingerprint["request"]
    assert isinstance(request, dict)
    recorded = request["headers"]
    assert isinstance(recorded, list)
    return _from_the_persona([(name, value) for name, _, value in (str(line).partition(": ") for line in recorded)])


def _built_for(baseline: Baseline) -> list[tuple[str, str]]:
    replay = REPLAYS[baseline.case]
    built = build_request(PERSONAS[baseline.release], replay.method, _ANY_URL, kind=replay.kind.value, **replay.body)
    return _as_recorded([(name, value) for name, value in built["headers"] if name.lower() != "origin" or replay.body])


_EVERY_HINT_CASES = {"client_hints_form", "client_hints_fetch_post", "client_hints_script", "client_hints_image"}
_EVERY_HINT_BASELINES = [item for item in ALL_BASELINES if item.case in _EVERY_HINT_CASES]


@pytest.mark.parametrize("baseline", [b for b in _EVERY_HINT_BASELINES if b.case == "client_hints_form"], ids=_name)
def test_a_navigation_holds_every_hint_in_the_order_and_with_the_values_of_the_baseline(baseline: Baseline) -> None:
    assert _built_for(baseline) == _recorded(baseline)


@pytest.mark.parametrize("baseline", [b for b in _EVERY_HINT_BASELINES if b.case != "client_hints_form"], ids=_name)
def test_a_subresource_holds_every_hint_with_the_value_of_the_baseline(baseline: Baseline) -> None:
    # Their order is that of Blink's map, which depends on which there are. So the native core works it out, and the
    # Parity Checks show that it does.
    assert sorted(_built_for(baseline)) == sorted(_recorded(baseline))


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
@pytest.mark.parametrize("kind", RequestKind)
def test_a_persona_claims_one_value_for_what_follows_the_machine(release: str, kind: RequestKind) -> None:
    lines = dict(PERSONAS[release].request_defaults(kind)["headers"])

    assert {name: lines[name] for name in HINTS_FROM_THE_MACHINE} == _CLAIMED_FROM_THE_MACHINE


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
@pytest.mark.parametrize("kind", RequestKind)
def test_a_persona_has_every_hint_a_server_can_ask_for_and_that_chrome_sends_of_a_kind(
    release: str, kind: RequestKind
) -> None:
    names = {name.lower() for name, _ in PERSONAS[release].request_defaults(kind)["headers"]}

    assert names >= HINTS_A_SERVER_ASKS_FOR | SENT_BY_DEFAULT
    assert names.isdisjoint({"width", "sec-ch-width", "save-data"})


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_the_full_version_is_the_build_that_recorded_the_baselines(release: str) -> None:
    note = (BASELINES_DIRECTORY / release / "chrome.txt").read_text(encoding="utf-8")
    build = re.search(r"^Chrome: (\S+)$", note, re.MULTILINE)
    assert build is not None
    lines = dict(PERSONAS[release].request_defaults(RequestKind.NAVIGATION)["headers"])

    assert lines["sec-ch-ua-full-version"] == f'"{build[1]}"'
    assert lines["sec-ch-ua-full-version-list"].count(f'v="{build[1]}"') == 2
