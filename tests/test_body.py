"""Tests of encoding a body. They need no native library and no connection, so they run on every machine.

The bodies that Chrome recorded come from the Baselines, and what those do not hold comes from a hand check in headless
Chrome 156 on macOS: a page's fetch() and forms sent these to a local server, and the tables here hold what arrived. Blink's
source at Chrome 157 says the same: FormData::EncodeMultiPartFormData() and FormDataEncoder.
"""

import re
from typing import Any

import pytest

from mitto._body import BodyEncoding, EncodedBody, FileFields, RequestData, encode_body, encode_query
from tools.parity import BASELINES_DIRECTORY, Baseline, load_baselines
from tools.parity.replay import REPLAYS

_BOUNDARY_PREFIX = "----WebKitFormBoundary"
_BOUNDARY = re.compile(rf"{_BOUNDARY_PREFIX}[0-9A-Za-z]{{16}}")

_BASELINES = [baseline for baseline in load_baselines(BASELINES_DIRECTORY) if REPLAYS[baseline.case].body]


def _name(baseline: Baseline) -> str:
    return f"{baseline.release}-{baseline.case}-{baseline.protocol}"


def _encode(data: RequestData | None = None, json_value: object = None, files: FileFields | None = None) -> EncodedBody:
    body = encode_body(data, json_value, files)
    assert body is not None
    return body


def _with_marker(body: EncodedBody) -> bytes:
    """Return a multipart body with `B` in place of its boundary, which is different each time."""
    return body.content.replace(body.boundary.encode(), b"B")


def test_the_recorded_cases_hold_a_body_of_each_kind_that_a_baseline_has() -> None:
    assert {baseline.case for baseline in _BASELINES} == {
        "navigation_form_urlencoded",
        "navigation_form_multipart",
        "fetch_post_text_same_origin",
        "fetch_post_text_cross_site",
        "fetch_post_json_same_origin",
    }


@pytest.mark.parametrize("baseline", _BASELINES, ids=_name)
def test_a_body_is_the_one_that_chrome_sent_in_the_baseline(baseline: Baseline) -> None:
    options = REPLAYS[baseline.case].body
    body = _encode(options.get("data"), options.get("json"), options.get("files"))
    request: Any = baseline.fingerprint["request"]

    text = body.content.decode("latin-1")
    if body.boundary:
        text = text.replace(body.boundary, f"{_BOUNDARY_PREFIX}<random>")
    assert text == request["body"]


def test_bytes_go_as_they_are_and_say_nothing_of_what_they_are() -> None:
    assert _encode(b"\x00\x01\x02\xff") == EncodedBody(b"\x00\x01\x02\xff", None)


@pytest.mark.parametrize("data", [bytearray(b"bytes"), memoryview(b"bytes")], ids=["bytearray", "memoryview"])
def test_anything_that_holds_bytes_goes_as_bytes(data: bytearray | memoryview) -> None:
    body = _encode(data)

    assert body.content == b"bytes"
    assert type(body.content) is bytes


@pytest.mark.parametrize(
    ("data", "encoding"),
    [(b"", None), ("", BodyEncoding.TEXT), ({}, BodyEncoding.FORM)],
    ids=["bytes", "text", "mapping"],
)
def test_an_empty_body_is_still_a_body(data: RequestData, encoding: BodyEncoding | None) -> None:
    assert _encode(data) == EncodedBody(b"", encoding)


def test_nothing_makes_no_body() -> None:
    assert encode_body(None, None, None) is None


@pytest.mark.parametrize(("value", "text"), [(0, b"0"), (False, b"false"), ("", b'""'), ([], b"[]"), ({}, b"{}")])
def test_a_json_value_that_is_false_in_python_is_still_a_body(value: object, text: bytes) -> None:
    assert _encode(json_value=value) == EncodedBody(text, BodyEncoding.JSON)


def test_text_goes_as_utf_8_as_it_is_written() -> None:
    body = _encode("text é \ud800 line\r\nline\nline\rend")

    assert body == EncodedBody(b"text \xc3\xa9 \xef\xbf\xbd line\r\nline\nline\rend", BodyEncoding.TEXT)


def test_a_mapping_goes_as_url_search_params_writes_it() -> None:
    body = _encode({"a b&c": "d e&f+g~*é", "\ud800": "😀"})

    assert body == EncodedBody(b"a+b%26c=d+e%26f%2Bg%7E*%C3%A9&%EF%BF%BD=%F0%9F%98%80", BodyEncoding.FORM)


def test_a_line_break_in_a_mapping_stays_as_url_search_params_keeps_it() -> None:
    assert encode_query([("a b&c", "x\ny")]) == "a+b%26c=x%0Ay"


def test_json_goes_as_json_stringify_writes_it() -> None:
    value: dict[str, object] = {"é": "\ud800", "a": [1, 1.5, 1e21, True, None, '\u2028\x7f\x01"\\/\n\t'], "😀": {}}

    body = _encode(json_value=value)

    assert body.encoding is BodyEncoding.JSON
    assert (
        body.content.decode() == '{"é":"\\ud800","a":[1,1.5,1e+21,true,null,"\u2028\x7f\\u0001\\"\\\\/\\n\\t"],"😀":{}}'
    )


@pytest.mark.parametrize("value", [float("nan"), float("inf"), [float("-inf")]], ids=["nan", "inf", "nested"])
def test_a_number_that_json_cannot_hold_is_refused(value: object) -> None:
    with pytest.raises(ValueError, match="JSON"):
        _encode(json_value=value)


def test_a_value_that_json_has_no_form_of_is_refused() -> None:
    with pytest.raises(TypeError, match="not JSON serializable"):
        _encode(json_value=object())


def test_a_multipart_body_escapes_as_blink_does() -> None:
    fields = {
        'quote"name': "plain",
        "lf\nname": "l1\nl2",
        "cr\rname": "l1\rl2",
        "crlf\r\nname": "l1\r\nl2",
        "é\ud800": "é\ud800",
        "percent%22": "%22",
        "semi;colon= x": "semi;colon= x",
    }
    files: dict[str, Any] = {
        "file1": ('q"uote\nlf\rcr é\ud800.txt', b"content\r\nkept\n", "Text/Plain; Charset=UTF-8"),
        "file2": ("no-type.bin", b"c"),
        "blob1": b"\x00\xff",
        "blob2": ("named.bin", b"x", "a/b"),
        "file3": ("bad-type.bin", b"c", "a/é"),
        "file4": ("bad-type2.bin", b"c", "a/b\r\nX: y"),
        "file5": ("", b"c"),
    }

    body = _encode(fields, files=files)

    assert body.encoding is BodyEncoding.MULTIPART
    assert _with_marker(body) == (
        b'--B\r\nContent-Disposition: form-data; name="quote%22name"\r\n\r\nplain\r\n'
        b'--B\r\nContent-Disposition: form-data; name="lf%0D%0Aname"\r\n\r\nl1\r\nl2\r\n'
        b'--B\r\nContent-Disposition: form-data; name="cr%0D%0Aname"\r\n\r\nl1\r\nl2\r\n'
        b'--B\r\nContent-Disposition: form-data; name="crlf%0D%0Aname"\r\n\r\nl1\r\nl2\r\n'
        b'--B\r\nContent-Disposition: form-data; name="\xc3\xa9\xef\xbf\xbd"\r\n\r\n\xc3\xa9\xef\xbf\xbd\r\n'
        b'--B\r\nContent-Disposition: form-data; name="percent%22"\r\n\r\n%22\r\n'
        b'--B\r\nContent-Disposition: form-data; name="semi;colon= x"\r\n\r\nsemi;colon= x\r\n'
        b'--B\r\nContent-Disposition: form-data; name="file1"; filename="q%22uote%0Alf%0Dcr \xc3\xa9\xef\xbf\xbd.txt"\r\n'
        b"Content-Type: text/plain; charset=utf-8\r\n\r\ncontent\r\nkept\n\r\n"
        b'--B\r\nContent-Disposition: form-data; name="file2"; filename="no-type.bin"\r\n'
        b"Content-Type: application/octet-stream\r\n\r\nc\r\n"
        b'--B\r\nContent-Disposition: form-data; name="blob1"; filename="blob"\r\n'
        b"Content-Type: application/octet-stream\r\n\r\n\x00\xff\r\n"
        b'--B\r\nContent-Disposition: form-data; name="blob2"; filename="named.bin"\r\n'
        b"Content-Type: a/b\r\n\r\nx\r\n"
        b'--B\r\nContent-Disposition: form-data; name="file3"; filename="bad-type.bin"\r\n'
        b"Content-Type: application/octet-stream\r\n\r\nc\r\n"
        b'--B\r\nContent-Disposition: form-data; name="file4"; filename="bad-type2.bin"\r\n'
        b"Content-Type: application/octet-stream\r\n\r\nc\r\n"
        b'--B\r\nContent-Disposition: form-data; name="file5"; filename=""\r\n'
        b"Content-Type: application/octet-stream\r\n\r\nc\r\n"
        b"--B--\r\n"
    )


def test_a_multipart_body_with_nothing_in_it_is_its_last_boundary() -> None:
    assert _with_marker(_encode(files={})) == b"--B--\r\n"


def test_files_can_repeat_a_field_name() -> None:
    body = _encode(files=[("file", ("a", b"1")), ("file", ("b", b"2"))])

    assert body.content.count(b'name="file"; filename="') == 2


def test_each_multipart_body_has_a_new_boundary_of_the_form_chrome_gives_it() -> None:
    boundaries = {_encode(files={}).boundary for _ in range(20)}

    assert len(boundaries) == 20
    assert all(_BOUNDARY.fullmatch(boundary) for boundary in boundaries)


@pytest.mark.parametrize(
    ("data", "json_value", "files"),
    [
        ("text", {"a": 1}, None),
        (None, {"a": 1}, {}),
        ("text", None, {}),
        (b"bytes", None, {}),
    ],
    ids=["json-and-data", "json-and-files", "text-and-files", "bytes-and-files"],
)
def test_arguments_that_do_not_go_together_are_refused(
    data: RequestData | None, json_value: object, files: FileFields | None
) -> None:
    with pytest.raises(ValueError, match=r"data|json"):
        encode_body(data, json_value, files)


@pytest.mark.parametrize("data", [1, [("a", "b")], object()], ids=["number", "pairs", "object"])
def test_data_of_another_type_is_refused(data: Any) -> None:
    with pytest.raises(TypeError, match="data"):
        encode_body(data, None, None)


@pytest.mark.parametrize("file", ["text", ("a",), (b"a", b"b"), ("a", "b"), ("a", b"b", 1)])
def test_a_file_of_another_shape_is_refused(file: Any) -> None:
    with pytest.raises(TypeError, match="file"):
        encode_body(None, None, {"file": file})


def test_the_bytes_of_a_body_stay_out_of_its_repr() -> None:
    assert "secret" not in repr(_encode(b"secret"))
