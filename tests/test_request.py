"""Tests of building a request. They need no native library and no connection, so they run on every machine."""

import re
from typing import Any
from urllib.parse import parse_qsl

import pytest
from hypothesis import given
from hypothesis import strategies as st

from mitto._body import encode_query
from mitto._persona import WINDOWS_PERSONAS, Persona, RequestKind, load_persona
from mitto._request import BodyOptions, BuiltRequest, build_request
from mitto._timeout import Timeout
from tests.client_hints import until_a_server_asks

URL = "https://example.test:8444/any/path"

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
        "referrer": None,
        "follow_redirects": True,
        "body": None,
        **persona.request_defaults(kind),
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

    assert built["headers"] == ENGINE_PERSONA.request_defaults(kind)["headers"]


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
    lines = persona.request_defaults(kind)["headers"]

    for index, (name, _) in enumerate(lines):
        # A different case on purpose: the name stays as Chrome spells it.
        built = build_request(persona, "GET", URL, kind=kind, headers={name.swapcase(): "changed"})

        assert built["headers"] == (*lines[:index], (name, "changed"), *lines[index + 1 :])


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_none_removes_a_personas_line(persona: Persona, kind: RequestKind) -> None:
    lines = persona.request_defaults(kind)["headers"]

    for index, (name, _) in enumerate(lines):
        built = build_request(persona, "GET", URL, kind=kind, headers={name.swapcase(): None})

        assert built["headers"] == (*lines[:index], *lines[index + 1 :])


@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize("kind", RequestKind)
def test_other_names_follow_the_personas_lines_in_the_order_given(persona: Persona, kind: RequestKind) -> None:
    lines = persona.request_defaults(kind)["headers"]

    built = build_request(persona, "GET", URL, kind=kind, headers={"X-Second": "2", "x-first": "1"})

    assert built["headers"] == (*lines, ("X-Second", "2"), ("x-first", "1"))


def test_none_for_a_name_the_persona_does_not_have_does_nothing() -> None:
    lines = ENGINE_PERSONA.request_defaults(RequestKind.NAVIGATION)["headers"]

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

    names = [name for name, _ in until_a_server_asks(built["headers"])]
    assert names == [
        "sec-ch-ua-platform",
        "User-Agent",
        "sec-ch-ua",
        "Accept",
        "Origin",
        "Sec-Fetch-Site",
        "Sec-Fetch-Mode",
        "Sec-Fetch-Dest",
        "X-New",
    ]
    assert ("Accept", "text/plain") in built["headers"]


def test_names_that_differ_only_in_case_are_refused() -> None:
    with pytest.raises(ValueError, match="differ only in case"):
        build_request(ENGINE_PERSONA, "GET", URL, headers={"X-Thing": "1", "x-thing": "2"})


@pytest.mark.parametrize(
    ("options", "message"),
    [
        ({"params": {"page": 2}}, "the value of 'page' in params has to be text, not int"),
        ({"params": {"page": 2.5}}, "the value of 'page' in params has to be text, not float"),
        ({"params": {"page": None}}, "the value of 'page' in params has to be text, not NoneType"),
        ({"params": {"page": b"2"}}, "the value of 'page' in params has to be text, not bytes"),
        ({"params": {"a": ["1", "2"]}}, "the value of 'a' in params has to be text, not list"),
        ({"params": {1: "a"}}, "a name in params has to be text, not int"),
        ({"params": {None: "a"}}, "a name in params has to be text, not NoneType"),
        ({"params": [("page", 2)]}, "the value of 'page' in params has to be text, not int"),
        ({"params": [(1, "a")]}, "a name in params has to be text, not int"),
        ({"params": [("a", "1"), ("b", 2)]}, "the value of 'b' in params has to be text, not int"),
        ({"params": ("ab",)}, "an item of params has to be a (name, value) pair, not str"),
        ({"params": [b"ab"]}, "an item of params has to be a (name, value) pair, not bytes"),
        ({"params": [1]}, "an item of params has to be a (name, value) pair, not int"),
        ({"params": [("a",)]}, "an item of params has to be a (name, value) pair, not tuple"),
        ({"params": [("a", "1", "2")]}, "an item of params has to be a (name, value) pair, not tuple"),
        ({"params": "a=b"}, "params has to be a mapping or (name, value) pairs, not str"),
        ({"params": ""}, "params has to be a mapping or (name, value) pairs, not str"),
        ({"params": b"a=b"}, "params has to be a mapping or (name, value) pairs, not bytes"),
        ({"params": 2}, "params has to be a mapping or (name, value) pairs, not int"),
        ({"data": {"a": 1}}, "the value of 'a' in data has to be text, not int"),
        ({"data": {"a": None}}, "the value of 'a' in data has to be text, not NoneType"),
        ({"data": {"a": b"1"}}, "the value of 'a' in data has to be text, not bytes"),
        ({"data": {1: "a"}}, "a name in data has to be text, not int"),
        ({"data": {"a": 1}, "files": {"f": b""}}, "the value of 'a' in data has to be text, not int"),
        ({"data": {1: "a"}, "files": {"f": b""}}, "a name in data has to be text, not int"),
        ({"files": {1: b""}}, "a name in files has to be text, not int"),
        ({"files": [(1, b"")]}, "a name in files has to be text, not int"),
        ({"files": [("f",)]}, "an item of files has to be a (name, value) pair, not tuple"),
        ({"files": "f"}, "files has to be a mapping or (name, value) pairs, not str"),
        ({"files": 1}, "files has to be a mapping or (name, value) pairs, not int"),
        ({"headers": {"X-A": 1}}, "the value of 'X-A' in headers has to be text or None, not int"),
        ({"headers": {"X-A": b"1"}}, "the value of 'X-A' in headers has to be text or None, not bytes"),
        ({"headers": {1: "a"}}, "a name in headers has to be text, not int"),
        ({"headers": {None: "a"}}, "a name in headers has to be text, not NoneType"),
        ({"headers": [("X-A", "1")]}, "headers has to be a mapping of names to text or None, not list"),
        ({"headers": []}, "headers has to be a mapping of names to text or None, not list"),
        ({"headers": "X-A: 1"}, "headers has to be a mapping of names to text or None, not str"),
        ({"headers": ""}, "headers has to be a mapping of names to text or None, not str"),
        ({"headers": 0}, "headers has to be a mapping of names to text or None, not int"),
        ({"timeout": (3, 10)}, "timeout has to be a number of seconds or a Timeout, not tuple"),
        ({"timeout": "5"}, "timeout has to be a number of seconds or a Timeout, not str"),
        ({"timeout": True}, "timeout has to be a number of seconds or a Timeout, not bool"),
        ({"timeout": False}, "timeout has to be a number of seconds or a Timeout, not bool"),
        ({"timeout": 1j}, "timeout has to be a number of seconds or a Timeout, not complex"),
        ({"method": 5}, "method has to be text, not int"),
        ({"method": b"POST"}, "method has to be text, not bytes"),
        ({"url": 5}, "url has to be text, not int"),
        ({"url": b"https://example.test/"}, "url has to be text, not bytes"),
        ({"kind": 5}, "kind has to be a RequestKind, its value or None, not int"),
        ({"initiator": 5}, "initiator has to be text or None, not int"),
        ({"initiator": b"https://example.test/"}, "initiator has to be text or None, not bytes"),
        ({"follow_redirects": "no"}, "follow_redirects has to be True or False, not str"),
        ({"follow_redirects": 0}, "follow_redirects has to be True or False, not int"),
        ({"follow_redirects": None}, "follow_redirects has to be True or False, not NoneType"),
        (
            {"files": {"f": "a,b"}},
            "the file 'f' in files has to be its content in bytes, (file name, content) or"
            " (file name, content, media type), not str",
        ),
        (
            {"files": [("f", ("report.csv", "a,b"))]},
            "the file 'f' in files has to be its content in bytes, (file name, content) or"
            " (file name, content, media type), not tuple",
        ),
        ({"json": {1}}, "json has to hold only what JSON has a form of: Object of type set is not JSON serializable"),
        (
            {"json": {"a": [b"1"]}},
            "json has to hold only what JSON has a form of: Object of type bytes is not JSON serializable",
        ),
        (
            {"json": {(1, 2): "a"}},
            "json has to hold only what JSON has a form of: keys must be str, int, float, bool or None, not tuple",
        ),
    ],
)
def test_an_argument_of_the_wrong_type_is_named_with_what_it_takes(options: dict[str, Any], message: str) -> None:
    arguments: dict[str, Any] = {"method": "POST", "url": URL, **options}

    with pytest.raises(TypeError) as caught:
        build_request(ENGINE_PERSONA, **arguments)

    assert str(caught.value) == message


@pytest.mark.parametrize(
    "options",
    [
        {"params": {"token": b"a secret"}},
        {"data": {"password": b"a secret"}},
        {"data": {"password": b"a secret"}, "files": {"f": b""}},
        {"headers": {"Authorization": b"a secret"}},
        {"files": {"report": "a secret"}},
        {"json": {"password": {"a secret"}}},
        {"initiator": b"https://a-secret.test/"},
    ],
)
def test_a_value_of_the_wrong_type_stays_out_of_the_message(options: dict[str, Any]) -> None:
    with pytest.raises(TypeError) as caught:
        build_request(ENGINE_PERSONA, "POST", URL, **options)

    assert "secret" not in str(caught.value)


@pytest.mark.parametrize(
    "params",
    [
        [["a", "1"], ["b", "2"]],
        (("a", "1"), ("b", "2")),
        iter([("a", "1"), ("b", "2")]),
        (pair for pair in [("a", "1"), ("b", "2")]),
    ],
    ids=["lists", "tuples", "iterator", "generator"],
)
def test_pairs_of_params_come_in_any_sequence_from_any_iterable(params: Any) -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL, params=params)["url"] == f"{URL}?a=1&b=2"


class _Text(str):
    __slots__ = ()


def test_a_subclass_of_str_counts_as_text() -> None:
    built = build_request(
        ENGINE_PERSONA,
        "POST",
        URL,
        params={_Text("a"): _Text("1")},
        data={_Text("b"): _Text("2")},
        headers={_Text("X-A"): _Text("3")},
    )

    assert built["url"] == f"{URL}?a=1"
    assert built["body"] == b"b=2"
    assert ("X-A", "3") in built["headers"]


@pytest.mark.parametrize("nothing", [None, {}])
def test_no_headers_changes_nothing(nothing: dict[str, str] | None) -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL, headers=nothing) == build_request(ENGINE_PERSONA, "GET", URL)


@pytest.mark.parametrize("nothing", [{}, [], ()], ids=["mapping", "list", "tuple"])
def test_no_params_leaves_the_url_as_it_is(nothing: Any) -> None:
    assert build_request(ENGINE_PERSONA, "GET", URL, params=nothing)["url"] == URL


@pytest.mark.parametrize("name", ["Referer", "referer", "REFERER"])
def test_a_referer_replaces_the_personas_and_never_goes_among_the_headers(name: str) -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, kind=RequestKind.FETCH, headers={name: "https://other.test/"})

    assert built["referrer"] == "https://other.test/"
    assert built["headers"] == ENGINE_PERSONA.request_defaults(RequestKind.FETCH)["headers"]
    assert built["fetch"].fixed_headers == (name.lower(),)


def test_a_referer_can_be_given_to_a_kind_that_has_none() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, headers={"Referer": "https://other.test/"})

    assert built["referrer"] == "https://other.test/"


def test_none_removes_what_the_initiator_would_make_the_referer() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, kind=RequestKind.FETCH, headers={"Referer": None})

    assert built["referrer"] is None
    assert built["fetch"].fixed_headers == ("referer",)


def test_with_no_referer_from_a_caller_the_initiator_decides_it() -> None:
    built = build_request(ENGINE_PERSONA, "GET", URL, kind=RequestKind.FETCH, initiator="https://other.test/page")

    assert built["referrer"] is None
    assert built["fetch"].initiator == "https://other.test/page"
    assert built["fetch"].fixed_headers == ()


def test_only_a_callers_names_are_fixed_and_not_what_a_body_says_of_itself() -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, data="x", headers={"X-Thing": "1", "Accept": None})

    assert built["fetch"].fixed_headers == ("x-thing", "accept")


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
        "body",
        "fetch",
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


def _names(built: BuiltRequest) -> list[str]:
    return [name for name, _ in built["headers"]]


def _line(built: BuiltRequest, name: str) -> str | None:
    return dict(built["headers"]).get(name)


def test_a_request_with_no_body_has_none_of_the_lines_of_a_body() -> None:
    for kind in (RequestKind.NAVIGATION, RequestKind.FETCH):
        built = build_request(ENGINE_PERSONA, "POST", URL, kind=kind)

        assert built["body"] is None
        assert {"Cache-Control", "Content-Type"}.isdisjoint(_names(built))


# Each Content-Type here came from fetch() or a form in Chrome 156, or from a Baseline.
@pytest.mark.parametrize("persona", PERSONAS, ids=_persona_id)
@pytest.mark.parametrize(
    ("kind", "options", "content_type"),
    [
        (RequestKind.FETCH, {"data": "text"}, "text/plain;charset=UTF-8"),
        (RequestKind.FETCH, {"data": ""}, "text/plain;charset=UTF-8"),
        (RequestKind.FETCH, {"data": {"a": "b"}}, "application/x-www-form-urlencoded;charset=UTF-8"),
        (RequestKind.FETCH, {"data": {}}, "application/x-www-form-urlencoded;charset=UTF-8"),
        (RequestKind.FETCH, {"data": b"bytes"}, None),
        (RequestKind.FETCH, {"data": b""}, None),
        (RequestKind.FETCH, {"json": {"a": 1}}, "application/json"),
        (RequestKind.NAVIGATION, {"data": "a b=c d\r\n"}, "text/plain"),
        (RequestKind.NAVIGATION, {"data": {"a": "b"}}, "application/x-www-form-urlencoded"),
        (RequestKind.NAVIGATION, {"data": {}}, "application/x-www-form-urlencoded"),
    ],
)
def test_a_body_has_the_content_type_chrome_gives_it(
    persona: Persona, kind: RequestKind, options: BodyOptions, content_type: str | None
) -> None:
    assert _line(build_request(persona, "POST", URL, kind=kind, **options), "Content-Type") == content_type


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.NAVIGATION])
def test_a_multipart_body_names_its_boundary_in_its_content_type(kind: RequestKind) -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, kind=kind, files={"file": b"content"})

    content_type = _line(built, "Content-Type")
    assert content_type is not None
    media_type, boundary = content_type.split("; boundary=")
    assert media_type == "multipart/form-data"
    assert isinstance(built["body"], bytes)
    assert built["body"].startswith(f"--{boundary}\r\n".encode())
    assert built["body"].endswith(f"--{boundary}--\r\n".encode())


def test_a_form_navigation_says_not_to_take_a_cached_answer_where_a_fetch_does_not() -> None:
    navigation = build_request(ENGINE_PERSONA, "POST", URL, kind=RequestKind.NAVIGATION, data={"a": "b"})
    fetch = build_request(ENGINE_PERSONA, "POST", URL, kind=RequestKind.FETCH, data={"a": "b"})

    assert _names(navigation)[0] == "Cache-Control"
    assert _line(navigation, "Cache-Control") == "max-age=0"
    assert _line(fetch, "Cache-Control") is None


def test_the_default_kind_of_a_post_with_a_body_is_a_fetch() -> None:
    assert build_request(ENGINE_PERSONA, "POST", URL, data="text") == build_request(
        ENGINE_PERSONA, "POST", URL, data="text", kind=RequestKind.FETCH
    )


@pytest.mark.parametrize(
    ("options", "body"),
    [
        ({"data": "é"}, b"\xc3\xa9"),
        ({"data": b"\x00"}, b"\x00"),
        ({"data": {"a": "b c"}}, b"a=b+c"),
        ({"json": [1]}, b"[1]"),
        ({"data": ""}, b""),
    ],
)
def test_a_body_goes_to_the_native_core_as_bytes(options: BodyOptions, body: bytes) -> None:
    assert build_request(ENGINE_PERSONA, "POST", URL, **options)["body"] == body


@pytest.mark.parametrize("name", ["Content-Type", "content-type", "CONTENT-TYPE"])
def test_a_callers_content_type_takes_the_place_of_the_body_s_and_keeps_the_personas_spelling(name: str) -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, data="text", headers={name: "text/csv"})

    assert _line(built, "Content-Type") == "text/csv"
    assert _names(built) == _names(build_request(ENGINE_PERSONA, "POST", URL, data="text"))


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.NAVIGATION])
def test_a_callers_content_type_for_bytes_goes_where_chrome_puts_it(kind: RequestKind) -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, kind=kind, data=b"x", headers={"Content-Type": "a/b"})

    assert _line(built, "Content-Type") == "a/b"
    assert _names(built) == _names(build_request(ENGINE_PERSONA, "POST", URL, kind=kind, data="text"))


def test_none_removes_the_content_type_of_a_body() -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, data="text", headers={"Content-Type": None})

    assert _line(built, "Content-Type") is None


def test_a_content_type_with_no_body_is_another_name_after_the_personas_lines() -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, headers={"Content-Type": "a/b"})

    assert built["headers"][-1] == ("Content-Type", "a/b")


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.NAVIGATION])
@pytest.mark.parametrize("options", [{"data": b"x"}, {"data": "x"}, {"json": 1}, {"files": {}}], ids=str)
def test_no_placeholder_of_a_persona_is_left_for_the_wire(kind: RequestKind, options: BodyOptions) -> None:
    built = build_request(ENGINE_PERSONA, "POST", URL, kind=kind, **options)

    # The native core writes the value of Origin, or removes the line. tests/test_initiator.py shows that on the wire.
    assert not [line for line in built["headers"] if "<" in line[1] and line[0] != "Origin"]


@pytest.mark.parametrize("method", ["GET", "get", "HEAD", "head"])
@pytest.mark.parametrize("options", [{"data": b""}, {"data": "x"}, {"json": 1}, {"files": {}}], ids=str)
def test_fetch_refuses_a_body_with_get_or_head_and_so_does_a_session(method: str, options: BodyOptions) -> None:
    with pytest.raises(ValueError, match="body"):
        build_request(ENGINE_PERSONA, method, URL, **options)


@pytest.mark.parametrize("kind", [RequestKind.SCRIPT, RequestKind.IMAGE])
def test_a_kind_that_chrome_sends_no_body_with_is_refused(kind: RequestKind) -> None:
    with pytest.raises(ValueError, match=kind.value):
        build_request(ENGINE_PERSONA, "POST", URL, kind=kind, data="x")


def test_a_caller_can_replace_or_remove_the_origin() -> None:
    replaced = build_request(ENGINE_PERSONA, "POST", URL, data="x", headers={"origin": "https://other.test"})
    removed = build_request(ENGINE_PERSONA, "POST", URL, data="x", headers={"Origin": None})

    assert _line(replaced, "Origin") == "https://other.test"
    assert replaced["fetch"].fixed_headers == ("origin",)
    assert _line(removed, "Origin") is None
    assert removed["fetch"].fixed_headers == ("origin",)
