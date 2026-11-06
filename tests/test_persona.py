"""Tests of Persona data against the Baselines of its Chrome Release.

They need no native library and no connection, so they run on every machine. What //net adds is written out here, and
the Parity Checks show that //net does add it.
"""

import itertools

import pytest

from mitto._native import Priority
from mitto._persona import WINDOWS_PERSONAS, RequestDefaults, RequestKind, load_persona
from tools.capture import HTTP1, HTTP2, HTTP3, KIND_PATHS
from tools.capture import RequestKind as CaptureRequestKind
from tools.capture.normal_form import AUTHORITY_PLACEHOLDER, ORIGIN_PLACEHOLDER
from tools.parity import BASELINES_DIRECTORY, Baseline, load_baselines

PERSONAS = {release: load_persona(name) for release, name in WINDOWS_PERSONAS.items()}
BASELINES = [
    baseline
    for baseline in load_baselines(BASELINES_DIRECTORY)
    if baseline.case in {kind.value for kind in RequestKind}
]

_ANY_URL = "https://example.test:8444/any/path?query"

# The default urgency, which //net leaves out of the priority header.
_DEFAULT_URGENCY = 3

# What //net makes of a request's priority: the urgency for the priority header, and the HTTP/2 stream weight.
_URGENCY_AND_WEIGHT = {Priority.HIGHEST: (0, 256), Priority.MEDIUM: (1, 220), Priority.LOWEST: (3, 147)}


def _defaults(baseline: Baseline) -> RequestDefaults:
    return PERSONAS[baseline.release].request_defaults(RequestKind(baseline.case), _ANY_URL)


def _priority_field(defaults: RequestDefaults) -> str:
    urgency, _ = _URGENCY_AND_WEIGHT[defaults["priority"]]
    parts = [f"u={urgency}"] if urgency != _DEFAULT_URGENCY else []
    return ", ".join([*parts, "i"] if defaults["priority_incremental"] else parts)


def _lines_on_the_wire(baseline: Baseline) -> list[str]:
    """Return the header lines that the Persona's data and //net together give for a Baseline's request."""
    defaults = _defaults(baseline)
    lines = [f"{name}: {value}" for name, value in defaults["headers"]]
    if defaults["referrer"] is not None:
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
def test_a_subresource_has_the_root_of_the_origin_of_its_url_as_its_referrer(release: str) -> None:
    defaults = PERSONAS[release].request_defaults(RequestKind.FETCH, _ANY_URL)

    assert defaults["referrer"] == "https://example.test:8444/"


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_a_navigation_has_no_referrer(release: str) -> None:
    assert PERSONAS[release].request_defaults(RequestKind.NAVIGATION, "https://example.test/")["referrer"] is None


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_the_persona_says_which_chrome_it_presents(release: str) -> None:
    assert (PERSONAS[release].release, PERSONAS[release].operating_system) == (release, "Windows")


def test_the_request_kinds_are_those_of_the_capture_kit() -> None:
    assert [kind.value for kind in RequestKind] == [kind.value for kind in CaptureRequestKind]
