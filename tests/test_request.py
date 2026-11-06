"""Tests of building a request. They need no native library and no connection, so they run on every machine."""

import re
from urllib.parse import parse_qsl

import pytest
from hypothesis import given
from hypothesis import strategies as st

from mitto._persona import WINDOWS_PERSONAS, Persona, RequestKind, load_persona
from mitto._request import build_request, encode_query
from mitto._timeout import Timeout

URL = "https://example.test:8444/any/path"
ORIGIN_ROOT = "https://example.test:8444/"

PERSONAS = [load_persona(name) for name in WINDOWS_PERSONAS.values()]
ENGINE_PERSONA = load_persona(WINDOWS_PERSONAS["157"])

# Everything that URLSearchParams leaves as it is, or writes in some other way than as a percent-encoded byte.
QUERY_TEXT = re.compile(r"[A-Za-z0-9*\-._+%&=]*")


def _persona_id(persona: Persona) -> str:
    return persona.release


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_a_request_of_a_kind_is_what_the_persona_sends_for_that_kind(persona: Persona, kind: RequestKind) -> None:
    assert build_request(persona, "GET", URL, kind=kind) == {
        "url": URL,
        "method": "GET",
        "timeout": Timeout(),
        "follow_redirects": True,
        **persona.request_defaults(kind, URL),
    }


@pytest.mark.parametrize("kind", RequestKind)
def test_a_kind_can_be_written_as_text(kind: RequestKind) -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL, kind=kind.value) == build_request(
        ENGINE_PERSONA, "GET", URL, kind=kind
    )


def test_an_unknown_kind_is_refused() -> None:
    with pytest.raises(ValueError, match="stylesheet"):
        build_request(ENGINE_PERSONA, "GET", URL, kind="stylesheet")


@pytest.mark.parametrize(
    ("method", "kind"),
    [
        ("GET", RequestKind.NAVIGATION),
        ("get", RequestKind.NAVIGATION),
        ("POST", RequestKind.FETCH),
        ("PUT", RequestKind.FETCH),
        ("PATCH", RequestKind.FETCH),
        ("DELETE", RequestKind.FETCH),
        ("HEAD", RequestKind.FETCH),
        ("OPTIONS", RequestKind.FETCH),
        ("PROPFIND", RequestKind.FETCH),
    ],
)
def test_the_default_kind_is_navigation_for_get_and_fetch_for_other_methods(method: str, kind: RequestKind) -> None:
    built = build_request(ENGINE_PERSONA, method, URL)

    assert built["headers"] == ENGINE_PERSONA.request_defaults(kind, URL)["headers"]


# Each result here came from new Request().method in Chrome 156.
@pytest.mark.parametrize(
    ("given", "sent"),
    [
        ("get", "GET"),
        ("Post", "POST"),
        ("pUT", "PUT"),
        ("delete", "DELETE"),
        ("head", "HEAD"),
        ("options", "OPTIONS"),
        ("PATCH", "PATCH"),
        ("patch", "patch"),
        ("PropFind", "PropFind"),
    ],
)
def test_a_method_is_normalized_as_fetch_does(given: str, sent: str) -> None:
    assert build_request(ENGINE_PERSONA, given, URL)["method"] == sent


# Each query here came from URLSearchParams.toString() in Chrome 156.
@pytest.mark.parametrize(
    ("params", "query"),
    [
        (
            {"k": "".join(map(chr, range(128)))},
            "k=%00%01%02%03%04%05%06%07%08%09%0A%0B%0C%0D%0E%0F%10%11%12%13%14%15%16%17%18%19%1A%1B%1C%1D%1E%1F"
            "+%21%22%23%24%25%26%27%28%29*%2B%2C-.%2F0123456789%3A%3B%3C%3D%3E%3F%40ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "%5B%5C%5D%5E_%60abcdefghijklmnopqrstuvwxyz%7B%7C%7D%7E%7F",
        ),
        ({"a b": "é€😀"}, "a+b=%C3%A9%E2%82%AC%F0%9F%98%80"),
        ({"\ud800": "x\udc00y"}, "%EF%BF%BD=x%EF%BF%BDy"),
        ([("a", "1"), ("a", "2"), ("", "")], "a=1&a=2&="),
        ({"first": "1", "second": "2"}, "first=1&second=2"),
        ({}, ""),
        ([], ""),
    ],
)
def test_params_encode_as_url_search_params_does(params: dict[str, str] | list[tuple[str, str]], query: str) -> None:
    assert encode_query(params) == query


@given(
    st.lists(
        st.tuples(
            st.text(st.characters(exclude_categories=["Cs"])),
            st.text(st.characters(exclude_categories=["Cs"])),
        ),
        min_size=1,
    )
)
def test_any_params_can_be_read_back_from_their_query(pairs: list[tuple[str, str]]) -> None:
    query = encode_query(pairs)

    assert QUERY_TEXT.fullmatch(query)
    assert parse_qsl(query, keep_blank_values=True) == pairs


@pytest.mark.parametrize(
    ("url", "with_query"),
    [
        ("https://example.test/", "https://example.test/?q=x"),
        ("https://example.test", "https://example.test?q=x"),
        ("https://example.test/?a=1", "https://example.test/?a=1&q=x"),
        ("https://example.test/?", "https://example.test/?q=x"),
        ("https://example.test/?a=1&", "https://example.test/?a=1&q=x"),
        ("https://example.test/#part", "https://example.test/?q=x#part"),
        ("https://example.test/?a=1#part?b", "https://example.test/?a=1&q=x#part?b"),
        ("https://example.test/#part?b", "https://example.test/?q=x#part?b"),
        ("https://example.test/a%20b?c=d+e", "https://example.test/a%20b?c=d+e&q=x"),
    ],
)
def test_params_join_what_the_url_has_and_change_nothing_else(url: str, with_query: str) -> None:
    assert build_request(ENGINE_PERSONA, "GET", url, params={"q": "x"})["url"] == with_query


@pytest.mark.parametrize("params", [None, {}, []])
def test_no_params_leave_the_url_alone(params: dict[str, str] | list[tuple[str, str]] | None) -> None:
    assert build_request(ENGINE_PERSONA, "GET", "https://example.test/?", params=params)["url"] == (
        "https://example.test/?"
    )


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_a_header_replaces_the_personas_line_in_place(persona: Persona, kind: RequestKind) -> None:
    lines = persona.request_defaults(kind, URL)["headers"]

    for index, (name, _) in enumerate(lines):
        # A different case on purpose: the name stays as Chrome spells it.
        built = build_request(persona, "GET", URL, kind=kind, headers={name.swapcase(): "changed"})

        assert built["headers"] == (*lines[:index], (name, "changed"), *lines[index + 1 :])


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_none_removes_a_personas_line(persona: Persona, kind: RequestKind) -> None:
    lines = persona.request_defaults(kind, URL)["headers"]

    for index, (name, _) in enumerate(lines):
        built = build_request(persona, "GET", URL, kind=kind, headers={name.swapcase(): None})

        assert built["headers"] == (*lines[:index], *lines[index + 1 :])


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_other_names_follow_the_personas_lines_in_the_order_given(persona: Persona, kind: RequestKind) -> None:
    lines = persona.request_defaults(kind, URL)["headers"]

    built = build_request(persona, "GET", URL, kind=kind, headers={"X-Second": "2", "x-first": "1"})

    assert built["headers"] == (*lines, ("X-Second", "2"), ("x-first", "1"))


def test_none_for_a_name_the_persona_does_not_have_does_nothing() -> None:
    lines = ENGINE_PERSONA.request_defaults(RequestKind.NAVIGATION, URL)["headers"]

    built = build_request(ENGINE_PERSONA, "GET", URL, headers={"Accept-Encoding": None, "X-Absent": None})

    assert built["headers"] == lines


def test_an_empty_value_is_a_value() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, headers={"Accept": "", "X-Empty": ""})

    assert ("Accept", "") in built["headers"]
    assert built["headers"][-1] == ("X-Empty", "")


def test_changes_to_several_lines_at_once() -> None:
    built = build_request(
        ENGINE_PERSONA,
        "GET",
        URL,
        kind=RequestKind.FETCH,
        headers={"X-New": "1", "Accept": "text/plain", "sec-ch-ua-mobile": None},
    )

    names = [name for name, _ in built["headers"]]
    assert names == [
        "sec-ch-ua-platform",
        "User-Agent",
        "sec-ch-ua",
        "Accept",
        "Sec-Fetch-Site",
        "Sec-Fetch-Mode",
        "Sec-Fetch-Dest",
        "X-New",
    ]
    assert ("Accept", "text/plain") in built["headers"]


def test_names_that_differ_only_in_case_are_refused() -> None:
    with pytest.raises(ValueError, match="differ only in case"):
        build_request(ENGINE_PERSONA, "GET", URL, headers={"X-Thing": "1", "x-thing": "2"})


@pytest.mark.parametrize("name", ["Referer", "referer", "REFERER"])
def test_a_referer_replaces_the_personas_and_never_goes_among_the_headers(name: str) -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, kind=RequestKind.FETCH, headers={name: "https://other.test/"})

    assert built["referrer"] == "https://other.test/"
    assert built["headers"] == ENGINE_PERSONA.request_defaults(RequestKind.FETCH, URL)["headers"]


def test_a_referer_can_be_given_to_a_kind_that_has_none() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, headers={"Referer": "https://other.test/"})

    assert built["referrer"] == "https://other.test/"


def test_none_removes_the_personas_referer() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, kind=RequestKind.FETCH, headers={"Referer": None})

    assert built["referrer"] is None


def test_the_personas_referer_names_the_origin_and_not_the_query() -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, params={"q": "x"})

    assert built["referrer"] == ORIGIN_ROOT


@pytest.mark.parametrize("url", ["not a url", "http://[bad/", "/only/a/path", "data:text/plain,hello"])
def test_a_url_without_an_origin_gets_no_referrer(url: str) -> None:
    # The native core would refuse the referrer and never say what is wrong with the URL.
    assert build_request(ENGINE_PERSONA, "POST", url)["referrer"] is None


@pytest.mark.parametrize("follow_redirects", [True, False])
def test_follow_redirects_goes_to_the_native_core(follow_redirects: bool) -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, follow_redirects=follow_redirects)

    assert built["follow_redirects"] is follow_redirects


def test_building_leaves_the_callers_headers_alone() -> None:
    headers = {"Accept": "text/plain", "Referer": None, "X-New": "1"}

    build_request(ENGINE_PERSONA, "GET", URL, headers=headers)

    assert headers == {"Accept": "text/plain", "Referer": None, "X-New": "1"}


def test_a_built_request_needs_no_argument_but_the_method_and_url() -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL)

    assert sorted(built) == [
        "follow_redirects",
        "headers",
        "method",
        "priority",
        "priority_incremental",
        "referrer",
        "timeout",
        "url",
    ]


def test_an_unknown_argument_is_refused() -> None:
    with pytest.raises(TypeError, match="retries"):
        build_request(ENGINE_PERSONA, "GET", URL, retries=1)  # pyright: ignore[reportCallIssue]


def test_no_timeout_runs_no_timer() -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL)["timeout"] == Timeout()
    assert build_request(ENGINE_PERSONA, "GET", URL, timeout=None)["timeout"] == Timeout()


def test_a_number_as_timeout_is_the_total_time() -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL, timeout=2.5)["timeout"] == Timeout(total=2.5)


def test_a_timeout_stays_as_it_is() -> None:
    timeout = Timeout(connect=1, read=2, total=3)

    assert build_request(ENGINE_PERSONA, "GET", URL, timeout=timeout)["timeout"] is timeout
