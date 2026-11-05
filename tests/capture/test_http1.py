"""Tests for the HTTP/1.1 side of the capture server."""

import pytest

from tools.capture.http1 import Http1Connection, parse_request_head
from tools.capture.page import KIND_PATHS, RequestKind, respond
from tools.capture.wire import ParseError

_GET = b"GET /fetch HTTP/1.1\r\nHost: example\r\nsec-ch-ua-mobile: ?0\r\nAccept:   */*  \r\n\r\n"


def test_keeps_names_order_and_values() -> None:
    request = parse_request_head(b'GET /a?b HTTP/1.1\r\nHost: x\r\nsec-ch-ua: "a"\r\nHost: y\r\nEmpty:')

    assert request.method == "GET"
    assert request.target == "/a?b"
    assert request.version == "HTTP/1.1"
    assert request.headers == (("Host", "x"), ("sec-ch-ua", '"a"'), ("Host", "y"), ("Empty", ""))


def test_keeps_every_byte_of_a_value() -> None:
    assert parse_request_head(b"GET / HTTP/1.1\r\nName: \xff\xe9").headers == (("Name", "\xff\xe9"),)


@pytest.mark.parametrize(
    "head",
    [
        b"",
        b"GET /",
        b"GET  / HTTP/1.1",
        b"GET / HTTP/2.0",
        b"GET / HTTP/1.1\r\nno colon",
        b"GET / HTTP/1.1\r\n: no name",
        b"GET / HTTP/1.1\r\nName : value",
        b"GET / HTTP/1.1\r\n folded: value",
        b"GET / HTTP/1.1\r\nName: bare\nline break",
        b"GET / HTTP/1.1\nName: value",
    ],
)
def test_refuses_malformed_heads(head: bytes) -> None:
    with pytest.raises(ParseError):
        parse_request_head(head)


def test_answers_each_request_and_stays_open() -> None:
    connection = Http1Connection(respond)

    assert connection.start() == b""
    output = connection.receive(_GET)

    assert output.startswith(b"HTTP/1.1 200 OK\r\n")
    assert b"\r\nConnection: keep-alive\r\n" in output
    assert b"\r\nCache-Control: no-store\r\n" in output
    assert output.endswith(b"\r\n\r\nfetched\n")
    assert not connection.closed
    assert connection.requests[0].headers == (("Host", "example"), ("sec-ch-ua-mobile", "?0"), ("Accept", "*/*"))


def test_handles_a_request_split_over_reads() -> None:
    connection = Http1Connection(respond)

    outputs = [connection.receive(_GET[position : position + 1]) for position in range(len(_GET))]

    assert all(output == b"" for output in outputs[:-1])
    assert outputs[-1].startswith(b"HTTP/1.1 200 OK\r\n")


def test_answers_pipelined_requests_in_order() -> None:
    connection = Http1Connection(respond)

    output = connection.receive(b"GET /fetch HTTP/1.1\r\n\r\nGET /missing HTTP/1.1\r\n\r\n")

    assert output.index(b"200 OK") < output.index(b"404 Not Found")
    assert [request.target for request in connection.requests] == ["/fetch", "/missing"]


def test_skips_a_body_by_its_length() -> None:
    connection = Http1Connection(respond)
    first = b"POST /fetch HTTP/1.1\r\nContent-Length: 5\r\n\r\nab"

    assert connection.receive(first).startswith(b"HTTP/1.1 200")
    assert connection.receive(b"cde" + b"GET /missing HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 404")


@pytest.mark.parametrize("header", [b"Connection: close", b"connection: CLOSE", b"Transfer-Encoding: chunked"])
def test_closes_when_it_has_to(header: bytes) -> None:
    connection = Http1Connection(respond)

    output = connection.receive(b"GET /fetch HTTP/1.1\r\n" + header + b"\r\n\r\n")

    assert b"\r\nConnection: close\r\n" in output
    assert connection.closed


@pytest.mark.parametrize("length", [b"-1", b"abc", b"\xb2", b""])
def test_refuses_a_bad_length(length: bytes) -> None:
    connection = Http1Connection(respond)

    with pytest.raises(ParseError):
        connection.receive(b"POST / HTTP/1.1\r\nContent-Length: " + length + b"\r\n\r\n")


def test_refuses_an_endless_head() -> None:
    connection = Http1Connection(respond)

    with pytest.raises(ParseError):
        connection.receive(b"GET / HTTP/1.1\r\n" + b"a" * (1 << 19))


def test_page_requests_every_request_kind() -> None:
    page = respond(KIND_PATHS[RequestKind.NAVIGATION]).body.decode()

    for kind in (RequestKind.FETCH, RequestKind.SCRIPT, RequestKind.IMAGE):
        assert KIND_PATHS[kind] in page
