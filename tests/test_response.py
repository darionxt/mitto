"""Tests of a Response and its headers. They need no native library and no connection, so they run on every machine."""

import dataclasses
import pickle

import pytest

from mitto import Headers, HTTPStatusError, MittoError, Redirect, Response
from mitto._native import NativeResponse, RedirectFollowed
from mitto._response import to_response

PAIRS = (
    ("Set-Cookie", "a=1"),
    ("X-Colon", "one: two"),
    ("X-Empty", ""),
    ("X-Latin-1", "caf\xe9"),
    ("set-cookie", "b=2, c=3"),
)


def response(content: bytes, *pairs: tuple[str, str]) -> Response:
    return Response(status=200, url="https://example.test/", protocol="h2", headers=Headers(pairs), content=content)


def test_iterating_headers_gives_each_pair_in_the_order_received() -> None:
    assert list(Headers(PAIRS)) == list(PAIRS)
    assert Headers(PAIRS).pairs == PAIRS
    assert len(Headers(PAIRS)) == len(PAIRS)


def test_headers_become_a_dict_with_the_last_value_of_a_name() -> None:
    assert dict(Headers((("A", "1"), ("A", "2"), ("B", "3")))) == {"A": "2", "B": "3"}


@pytest.mark.parametrize("name", ["x-colon", "X-Colon", "X-COLON"])
def test_lookup_ignores_case(name: str) -> None:
    headers = Headers(PAIRS)

    assert headers[name] == "one: two"
    assert headers.get(name) == "one: two"
    assert headers.get_all(name) == ["one: two"]
    assert name in headers


def test_a_repeated_header_comes_back_joined_or_one_by_one() -> None:
    headers = Headers(PAIRS)

    assert headers["SET-COOKIE"] == "a=1, b=2, c=3"
    assert headers.get("set-cookie") == "a=1, b=2, c=3"
    assert headers.get_all("Set-Cookie") == ["a=1", "b=2, c=3"]


def test_a_missing_header_is_not_found() -> None:
    headers = Headers(PAIRS)

    with pytest.raises(KeyError, match="X-Absent"):
        headers["X-Absent"]
    assert headers.get("X-Absent") is None
    assert headers.get("X-Absent", "fallback") == "fallback"
    assert headers.get_all("X-Absent") == []
    assert "X-Absent" not in headers


def test_an_empty_value_is_found() -> None:
    headers = Headers(PAIRS)

    assert headers["X-Empty"] == ""
    assert headers.get("X-Empty", "fallback") == ""
    assert "X-Empty" in headers


def test_headers_compare_in_order_and_with_the_case_received() -> None:
    assert Headers((("A", "1"), ("B", "2"))) == Headers((("A", "1"), ("B", "2")))
    assert Headers((("A", "1"), ("B", "2"))) != Headers((("B", "2"), ("A", "1")))
    assert Headers((("A", "1"),)) != Headers((("a", "1"),))


@pytest.mark.parametrize(
    ("content", "content_type", "text"),
    [
        pytest.param("café".encode(), None, "café", id="no-header"),
        pytest.param("café".encode(), "text/plain", "café", id="no-charset"),
        pytest.param(b"caf\xe9", "text/plain; charset=iso-8859-1", "café", id="latin-1"),
        pytest.param(b"caf\xe9", 'text/plain; charset="iso-8859-1"', "café", id="quoted"),
        pytest.param(b"caf\xe9", "text/plain;CHARSET=ISO-8859-1", "café", id="upper-case"),
        pytest.param(b"caf\xe9", "text/plain ;  charset=iso-8859-1 ", "café", id="spaces"),
        pytest.param(b"caf\xe9", "text/plain; format=flowed; charset=iso-8859-1", "café", id="second-parameter"),
        pytest.param(b"\xff\xfeh\x00i\x00", "text/plain; charset=utf-16", "hi", id="utf-16"),
        pytest.param(b"a\xffb", None, "a�b", id="no-header-replaces"),
        pytest.param(b"a\xffb", "text/plain", "a�b", id="no-charset-replaces"),
        pytest.param(b"a\xffb", "text/plain; charset=ascii", "a�b", id="declared-charset-replaces"),
        pytest.param("café".encode(), "text/plain; charset=no-such-charset", "café", id="unknown"),
        pytest.param(b"a\xffb", "text/plain; charset=no-such-charset", "a�b", id="unknown-replaces"),
        pytest.param("café".encode(), "text/plain; charset=", "café", id="empty"),
        pytest.param("café".encode(), 'text/plain; charset=""', "café", id="empty-quoted"),
        # Python has these codecs and none of them can do this.
        pytest.param("café".encode(), "text/plain; charset=zlib", "café", id="not-for-text"),
        pytest.param("café".encode(), "text/plain; charset=rot13", "café", id="text-to-text"),
        pytest.param("café".encode(), "text/plain; charset=idna", "café", id="refuses-to-replace"),
        pytest.param("café".encode(), "text/plain; charset=undefined", "café", id="always-fails"),
        pytest.param(b"", "text/plain", "", id="no-body"),
    ],
)
def test_text_uses_the_charset_in_content_type_and_otherwise_utf8_with_replacement(
    content: bytes, content_type: str | None, text: str
) -> None:
    pairs = [("Content-Type", content_type)] if content_type is not None else []

    assert response(content, *pairs).text == text


def test_text_finds_content_type_in_any_case() -> None:
    assert response(b"caf\xe9", ("content-TYPE", "text/plain; charset=iso-8859-1")).text == "café"


def test_text_uses_the_last_content_type() -> None:
    pairs = (("Content-Type", "text/plain; charset=utf-16"), ("Content-Type", "text/plain; charset=iso-8859-1"))

    assert response(b"caf\xe9", *pairs).text == "café"


@pytest.mark.parametrize(
    ("content", "value"),
    [
        (b'{"a": [1, 2.5, "b", null, true]}', {"a": [1, 2.5, "b", None, True]}),
        ('"é"'.encode(), "é"),
        ('"é"'.encode("utf-8-sig"), "é"),
        ('"é"'.encode("utf-16"), "é"),
    ],
)
def test_json_parses_the_body(content: bytes, value: object) -> None:
    assert response(content).json() == value


def test_json_does_not_go_by_the_charset_in_content_type() -> None:
    assert response('"é"'.encode(), ("Content-Type", "application/json; charset=iso-8859-1")).json() == "é"


@pytest.mark.parametrize("content", [b"", b"not json", b'"\xff"'])
def test_json_raises_value_error_on_what_is_not_json(content: bytes) -> None:
    with pytest.raises(ValueError):  # noqa: PT011 - the kinds of ValueError are what the test is about.
        response(content).json()


def test_the_body_stays_out_of_repr() -> None:
    assert "a secret body" not in repr(response(b"a secret body"))


def test_a_response_cannot_be_changed() -> None:
    with pytest.raises(dataclasses.FrozenInstanceError):
        response(b"").status = 404  # type: ignore[misc]  # pyright: ignore[reportAttributeAccessIssue]


def test_a_native_response_becomes_a_response() -> None:
    native = NativeResponse(status=201, protocol="h3", url="https://example.test/a", headers=PAIRS, body=b"body")

    assert to_response(native) == Response(
        status=201, url="https://example.test/a", protocol="h3", headers=Headers(PAIRS), content=b"body"
    )


@pytest.mark.parametrize("from_cache", [False, True])
def test_a_response_says_whether_it_came_from_the_cache(from_cache: bool) -> None:
    native = NativeResponse(
        status=200, protocol="h2", url="https://example.test/a", headers=(), body=b"", from_cache=from_cache
    )

    assert to_response(native).from_cache is from_cache


def with_status(status: int) -> Response:
    return dataclasses.replace(response(b"body"), status=status)


@pytest.mark.parametrize("status", [200, 204, 301, 304, 399])
def test_a_status_that_is_no_failure_raises_nothing(status: int) -> None:
    with_status(status).raise_for_status()


@pytest.mark.parametrize("status", [400, 404, 451, 500, 503, 599, 999])
def test_a_status_that_says_the_request_failed_raises(status: int) -> None:
    failed = with_status(status)

    with pytest.raises(HTTPStatusError) as caught:
        failed.raise_for_status()

    assert isinstance(caught.value, MittoError)
    assert caught.value.response is failed
    assert caught.value.status == status
    assert caught.value.url == failed.url
    assert str(status) in str(caught.value)
    assert failed.url in str(caught.value)


def test_a_status_error_survives_pickling() -> None:
    restored = pickle.loads(pickle.dumps(HTTPStatusError(with_status(404))))

    assert restored.response == with_status(404)
    assert restored.status == 404


def test_the_public_response_holds_each_redirect() -> None:
    hops = (
        RedirectFollowed(status=301, url="https://example.test/a", headers=(("Location", "/b"),)),
        RedirectFollowed(status=307, url="https://example.test/b", headers=(("Location", "/c"),)),
    )
    native = NativeResponse(status=200, protocol="h2", url="https://example.test/c", headers=(), body=b"", history=hops)

    assert to_response(native).history == (
        Redirect(status=301, url="https://example.test/a", headers=Headers((("Location", "/b"),))),
        Redirect(status=307, url="https://example.test/b", headers=Headers((("Location", "/c"),))),
    )


def test_a_response_with_no_redirects_has_an_empty_history() -> None:
    assert response(b"").history == ()
