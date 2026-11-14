"""Tests for the public API.

Tests that take the native_library fixture go through the real native core to a local server, so they need no network
connection, and are skipped when this machine has no native library.
"""

import dataclasses
import functools
import http.server
import inspect
import json
import operator
import os
import re
import subprocess
import sys
import threading
import typing
from collections.abc import AsyncIterable, Buffer, Callable, Iterator, Sequence
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto import _native
from mitto._body import FileFields, QueryParams, RequestData
from mitto._cookies import CookiePriority, CookieSourceScheme, CookieSourceType, SameSite
from mitto._machine import (
    ColorScheme,
    ContrastTheme,
    InvertedColors,
    ReducedMotion,
    ReducedTransparency,
    WindowState,
)
from mitto._native import PrivateSettings
from mitto._persona import ENGINE_RELEASE_PERSONA, WINDOWS_PERSONAS, RequestKind, load_persona
from mitto._request import BodyOptions, RequestHeaders
from tests.client_hints import until_a_server_asks
from tests.local_server import LocalHTTPServer
from tools.capture import HTTP1, HTTP2, HTTP3, KIND_PATHS, MAIN_CASES, CaptureServer, normal_form
from tools.capture import RequestKind as CaptureRequestKind

pytestmark = pytest.mark.usefixtures("both_apis")

ERR_INVALID_ARGUMENT = -4
ERR_INVALID_URL = -300

# What //net advertises to localhost, which is where the test server is.
ACCEPT_ENCODING = ("Accept-Encoding", "gzip, deflate, br, zstd")

# Every method of a Session, and what it sends.
METHODS = {
    "get": "GET",
    "post": "POST",
    "put": "PUT",
    "patch": "PATCH",
    "delete": "DELETE",
    "head": "HEAD",
    "options": "OPTIONS",
}

# The NUL shows that the body is read by its size, not as a C string.
BODY = b"a body with a \x00 inside"

# Written to the socket as they are, so that the expected headers do not depend on what http.server adds. The value
# with a byte above 0x7f is not valid UTF-8, and 0x85 is a line break to str.splitlines().
RAW_HEADERS = (
    b"Set-Cookie: a=1\r\n"
    b"X-Colon: one: two\r\n"
    b"X-Empty:\r\n"
    b"X-Latin-1: caf\xe9 \x85 end\r\n"
    b"Set-Cookie: b=2\r\n"
    b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
)


class RawResponseHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        status_line = b"HTTP/1.0 404 Not Found" if self.path == "/missing" else b"HTTP/1.0 203 Non-Authoritative"
        self.wfile.write(status_line + b"\r\n" + RAW_HEADERS + b"\r\n" + BODY)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server_url() -> Iterator[str]:
    """Run a local HTTP server and return its URL."""
    with LocalHTTPServer(("127.0.0.1", 0), RawResponseHandler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}/"
        server.shutdown()


@pytest.mark.usefixtures("native_library")
def test_get_returns_the_status(server_url: str) -> None:
    assert mitto.get(server_url).status == 203


@pytest.mark.usefixtures("native_library")
def test_get_returns_headers_in_the_order_received(server_url: str) -> None:
    assert mitto.get(server_url).headers.pairs == (
        ("Set-Cookie", "a=1"),
        ("X-Colon", "one: two"),
        ("X-Empty", ""),
        ("X-Latin-1", "caf\xe9 \x85 end"),
        ("Set-Cookie", "b=2"),
        ("Content-Length", str(len(BODY))),
    )


@pytest.mark.usefixtures("native_library")
def test_get_returns_the_body_as_bytes(server_url: str) -> None:
    assert mitto.get(server_url).content == BODY


@pytest.mark.usefixtures("native_library")
def test_get_returns_an_error_status_as_a_response(server_url: str) -> None:
    assert mitto.get(f"{server_url}missing").status == 404


@pytest.mark.usefixtures("native_library")
def test_get_raises_request_error_with_the_net_error() -> None:
    with pytest.raises(mitto.RequestError) as caught:
        mitto.get("not a url")

    assert caught.value.url == "not a url"
    assert caught.value.net_error == ERR_INVALID_URL
    assert str(ERR_INVALID_URL) in str(caught.value)


def test_get_rejects_a_url_with_a_nul_before_loading_the_library(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    # The C API would stop reading at the NUL and fetch a different URL.
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(ValueError, match="NUL"):
        mitto.get("http://127.0.0.1/\0.example.test/")


def test_get_without_a_library_raises_native_library_error(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(mitto.NativeLibraryError, match=_native.LIBRARY_ENV_VAR):
        mitto.get("http://127.0.0.1/")


def test_import_does_not_load_the_library(tmp_path: Path) -> None:
    # Naming a missing file makes any attempt to load the library fail.
    env = os.environ | {_native.LIBRARY_ENV_VAR: str(tmp_path / "missing")}

    subprocess.run([sys.executable, "-c", "import mitto"], env=env, check=True)


@dataclass(frozen=True, slots=True)
class ReceivedRequest:
    """What the server saw of one request."""

    method: str
    path: str
    headers: list[tuple[str, str]]
    body: bytes = b""

    def header(self, name: str) -> str | None:
        return dict(self.headers).get(name)


REDIRECTS = {"/redirect": 302, "/redirect-307": 307}


class AnyMethodServer(LocalHTTPServer):
    """An HTTP/1.1 server that takes every method a Session has, and records each request."""

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, AnyMethodHandler)
        self.received: list[ReceivedRequest] = []

    @property
    def authority(self) -> str:
        return f"127.0.0.1:{self.server_port}"

    @property
    def url(self) -> str:
        return f"http://{self.authority}/"


class AnyMethodHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(AnyMethodServer, self.server)
        body_size = int(self.headers.get("Content-Length", "0"))
        server.received.append(
            ReceivedRequest(self.command, self.path, self.headers.items(), self.rfile.read(body_size))
        )
        if self.path in REDIRECTS:
            self.send_response(REDIRECTS[self.path])
            self.send_header("Location", "/final")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = b""
        content_type = None
        if self.path == "/json":
            body, content_type = json.dumps({"key": ["value", 1]}).encode(), "application/json"
        elif self.path == "/latin-1":
            body, content_type = b"caf\xe9", "text/plain; charset=iso-8859-1"
        self.send_response(201)
        self.send_header("X-Repeated", "one")
        self.send_header("X-Repeated", "two")
        if content_type is not None:
            self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self) -> None:
        self.answer()

    def do_POST(self) -> None:
        self.answer()

    def do_PUT(self) -> None:
        self.answer()

    def do_PATCH(self) -> None:
        self.answer()

    def do_DELETE(self) -> None:
        self.answer()

    def do_HEAD(self) -> None:
        self.answer()

    def do_OPTIONS(self) -> None:
        self.answer()

    def do_PROPFIND(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[AnyMethodServer]:
    """Run a local server that takes every method."""
    with AnyMethodServer(("127.0.0.1", 0)) as running:
        threading.Thread(target=running.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield running
        running.shutdown()


def on_the_wire(server: AnyMethodServer, persona_name: str, kind: RequestKind) -> list[tuple[str, str]]:
    """Return what a GET to the server sends for a Persona and a Request Kind, when the caller changes nothing.

    A subresource comes from the page at the root of its URL's origin, and a navigation from no page. An element in cors
    mode keeps the Origin that Blink gives it, also for its own origin, and a worker's script gets its User-Agent from
    //net, after Referer.
    """
    persona = load_persona(persona_name)
    fetch = persona.request_defaults(kind)["fetch"]
    keeps_origin = fetch.mode == "cors" and fetch.destination != "empty"
    origin = server.url.removesuffix("/")
    lines = [
        (name, origin if name == "Origin" else value)
        for name, value in until_a_server_asks(persona.request_defaults(kind)["headers"])
        if name != "Origin" or keeps_origin
    ]
    return [
        ("Host", server.authority),
        ("Connection", "keep-alive"),
        *lines,
        *([("Referer", server.url)] if kind != RequestKind.NAVIGATION else []),
        *([("User-Agent", persona.user_agent)] if kind == RequestKind.WORKER else []),
        ACCEPT_ENCODING,
        ("Accept-Language", persona.accept_language),
    ]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("name", "method"), METHODS.items())
def test_each_method_sends_its_http_method(server: AnyMethodServer, name: str, method: str) -> None:
    with mitto.Session() as session:
        response = getattr(session, name)(server.url)

    assert response.status == 201
    assert server.received[0].method == method


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("name", "method"), METHODS.items())
def test_each_method_takes_each_argument(server: AnyMethodServer, name: str, method: str) -> None:
    with mitto.Session() as session:
        getattr(session, name)(server.url, params={"q": "a b"}, headers={"X-Thing": "1"}, kind="script")

    received = server.received[0]
    assert (received.method, received.path) == (method, "/?q=a+b")
    assert received.header("X-Thing") == "1"
    assert received.header("Sec-Fetch-Dest") == "script"


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("name", "method"), METHODS.items())
def test_request_does_what_the_method_of_that_name_does(server: AnyMethodServer, name: str, method: str) -> None:
    with mitto.Session() as session:
        getattr(session, name)(server.url)
        session.request(method, server.url)

    first, second = server.received
    assert first == second


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("name", "mode"), [("get", "navigate"), *((name, "cors") for name in METHODS if name != "get")]
)
def test_the_default_kind_is_navigation_for_get_and_fetch_for_other_methods(
    server: AnyMethodServer, name: str, mode: str
) -> None:
    with mitto.Session() as session:
        getattr(session, name)(server.url)

    assert server.received[0].header("Sec-Fetch-Mode") == mode


@pytest.mark.usefixtures("native_library")
def test_request_sends_a_method_with_no_function_of_its_own(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.request("PROPFIND", server.url)

    assert server.received[0].method == "PROPFIND"


@pytest.mark.usefixtures("native_library")
def test_a_method_that_chrome_never_sends_is_refused(server: AnyMethodServer) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.request("TRACE", server.url)

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert server.received == []


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("persona", WINDOWS_PERSONAS.values())
@pytest.mark.parametrize("kind", RequestKind)
def test_a_session_presents_its_persona_and_the_kind_it_is_given(
    server: AnyMethodServer, persona: str, kind: RequestKind
) -> None:
    with mitto.Session(persona=persona) as session:
        session.get(server.url, kind=kind)

    assert server.received[0].headers == on_the_wire(server, persona, kind)


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("release", "persona"), WINDOWS_PERSONAS.items())
def test_a_session_says_the_chrome_release_of_its_persona(server: AnyMethodServer, release: str, persona: str) -> None:
    with mitto.Session(persona=persona) as session:
        session.get(server.url)

    assert f"Chrome/{release}.0.0.0" in (server.received[0].header("User-Agent") or "")


@pytest.mark.usefixtures("native_library")
def test_the_default_persona_is_the_engine_releases(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.get(server.url)

    assert WINDOWS_PERSONAS["157"] == ENGINE_RELEASE_PERSONA
    assert server.received[0].headers == on_the_wire(server, ENGINE_RELEASE_PERSONA, RequestKind.NAVIGATION)


@pytest.mark.parametrize("persona", ["chrome_154_windows", "../chrome_157_windows", ""])
def test_an_unknown_persona_is_refused_before_loading_the_library(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, persona: str
) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(ValueError, match="no Persona"):
        mitto.Session(persona=persona)


def test_a_session_without_a_library_raises_native_library_error(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(mitto.NativeLibraryError, match=_native.LIBRARY_ENV_VAR):
        mitto.Session()


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("persona", WINDOWS_PERSONAS.values())
def test_accept_language_overrides_the_personas(server: AnyMethodServer, persona: str) -> None:
    with mitto.Session(persona=persona, accept_language="fr-CH, fr;q=0.9") as session:
        session.get(server.url)

    assert server.received[0].headers == [
        *on_the_wire(server, persona, RequestKind.NAVIGATION)[:-1],
        ("Accept-Language", "fr-CH, fr;q=0.9"),
    ]


@pytest.mark.usefixtures("native_library")
def test_an_empty_accept_language_sends_none(server: AnyMethodServer) -> None:
    with mitto.Session(accept_language="") as session:
        session.get(server.url)

    assert server.received[0].header("Accept-Language") is None


@pytest.mark.usefixtures("native_library")
def test_an_invalid_accept_language_is_refused() -> None:
    with pytest.raises(ValueError, match="refused"):
        mitto.Session(accept_language="a\r\nX-Injected: b")


@pytest.mark.usefixtures("native_library")
def test_params_reach_the_wire_as_url_search_params_writes_them(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}?a=1#part", params={"q": "é ~*"})

    assert server.received[0].path == "/?a=1&q=%C3%A9+%7E*"
    assert response.url == f"{server.url}?a=1&q=%C3%A9+%7E*#part"


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("persona", WINDOWS_PERSONAS.values())
def test_a_callers_headers_reach_the_wire_where_the_persona_and_net_put_them(
    server: AnyMethodServer, persona: str
) -> None:
    expected = on_the_wire(server, persona, RequestKind.FETCH)
    accept = expected.index(("Accept", "*/*"))
    expected[accept] = ("Accept", "text/plain")
    expected.remove(("sec-ch-ua-mobile", "?0"))
    referer = next(index for index, (name, _) in enumerate(expected) if name == "Referer")
    expected[referer] = ("Referer", "http://example.test/page")
    expected[referer:referer] = [("X-Thing", "1")]

    with mitto.Session(persona=persona) as session:
        session.get(
            server.url,
            kind="fetch",
            headers={
                "X-Thing": "1",
                "accept": "text/plain",
                "Sec-CH-UA-Mobile": None,
                "referer": "http://example.test/page",
            },
        )

    assert server.received[0].headers == expected


@pytest.mark.usefixtures("native_library")
def test_none_removes_the_personas_referer_from_the_wire(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.get(server.url, kind="fetch", headers={"Referer": None})

    assert server.received[0].header("Referer") is None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("persona", WINDOWS_PERSONAS.values())
def test_none_for_the_user_agent_leaves_the_personas_for_net_to_add_in_its_own_place(
    server: AnyMethodServer, persona: str
) -> None:
    with mitto.Session(persona=persona) as session:
        session.get(server.url, headers={"User-Agent": None})

    lines = on_the_wire(server, persona, RequestKind.NAVIGATION)
    line = ("User-Agent", load_persona(persona).user_agent)
    without = [item for item in lines if item != line]
    # //net adds it before the two that come last.
    assert server.received[0].headers == [*without[:-2], line, *without[-2:]]


@pytest.mark.usefixtures("native_library")
def test_an_accept_language_among_the_headers_replaces_the_sessions(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.get(server.url, headers={"Accept-Language": "fr"})

    assert [value for name, value in server.received[0].headers if name == "Accept-Language"] == ["fr"]


@pytest.mark.usefixtures("native_library")
def test_a_header_that_is_not_valid_is_refused(server: AnyMethodServer) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.get(server.url, headers={"X-Thing": "a\r\nX-Injected: b"})

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert server.received == []


@pytest.mark.usefixtures("native_library")
def test_a_header_that_cannot_be_encoded_is_refused(server: AnyMethodServer) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="latin-1"):
        session.get(server.url, headers={"X-Thing": "€"})


@pytest.mark.usefixtures("native_library")
def test_a_response_says_what_arrived(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}json")

    assert response.status == 201
    assert response.url == f"{server.url}json"
    assert response.protocol == "http/1.1"
    assert response.headers["content-type"] == "application/json"
    assert response.headers["x-repeated"] == "one, two"
    assert response.headers.get_all("X-REPEATED") == ["one", "two"]
    assert [name for name, _ in response.headers][-3:] == ["X-Repeated", "Content-Type", "Content-Length"]
    assert response.content == b'{"key": ["value", 1]}'
    assert response.text == '{"key": ["value", 1]}'
    assert response.json() == {"key": ["value", 1]}


@pytest.mark.usefixtures("native_library")
def test_a_response_decodes_its_text_with_its_charset(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        assert session.get(f"{server.url}latin-1").text == "café"


@pytest.mark.usefixtures("native_library")
def test_a_response_to_head_has_no_body(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        response = session.head(f"{server.url}json")

    assert response.headers["Content-Length"] == "21"
    assert response.content == b""


@pytest.mark.usefixtures("native_library")
def test_a_response_gives_the_url_it_ended_at(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}redirect")

    assert response.url == f"{server.url}final"
    assert response.status == 201
    assert [(hop.status, hop.url) for hop in response.history] == [(302, f"{server.url}redirect")]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("route", "protocol"), [(HTTP1, "http/1.1"), (HTTP2, "h2"), (HTTP3, "h3")])
def test_a_session_takes_private_settings_and_says_the_protocol(route: str, protocol: str) -> None:
    with (
        CaptureServer() as capture_server,
        mitto.Session(
            _settings=PrivateSettings(
                ignore_certificate_errors_spki_list=[capture_server.spki_hash],
                origins_to_force_quic_on=[capture_server.authority(HTTP3)],
            )
        ) as session,
    ):
        response = session.get(f"https://{capture_server.authority(route)}{KIND_PATHS[CaptureRequestKind.FETCH]}")

    assert response.protocol == protocol


@pytest.mark.usefixtures("native_library")
def test_a_session_closes_at_the_end_of_a_with_statement(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.get(server.url)

    with pytest.raises(mitto.RequestError):
        session.get(server.url)


@pytest.mark.usefixtures("native_library")
def test_closing_a_session_twice_does_no_harm() -> None:
    session = mitto.Session()

    session.close()
    session.close()


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("name", METHODS)
def test_an_invalid_url_is_reported_by_each_method(name: str) -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        getattr(session, name)("not a url")

    assert caught.value.net_error == ERR_INVALID_URL
    assert caught.value.method == METHODS[name]


@pytest.mark.usefixtures("native_library")
def test_get_takes_the_arguments_of_a_sessions_get(server: AnyMethodServer) -> None:
    response = mitto.get(server.url, params={"q": "x"}, headers={"X-Thing": "1"}, kind="image")

    received = server.received[0]
    assert response.url == f"{server.url}?q=x"
    assert received.path == "/?q=x"
    assert received.header("X-Thing") == "1"
    assert received.header("Sec-Fetch-Dest") == "image"


@pytest.mark.usefixtures("native_library")
def test_get_sends_what_a_default_session_sends(server: AnyMethodServer) -> None:
    mitto.get(server.url)
    with mitto.Session() as session:
        session.get(server.url)

    first, second = server.received
    assert first == second


def _lines_of(request: ReceivedRequest) -> list[str]:
    return [name for name, _ in request.headers]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("options", "body", "content_type"),
    [
        ({"data": b"\x00bytes\xff"}, b"\x00bytes\xff", None),
        ({"data": "text é"}, b"text \xc3\xa9", "text/plain;charset=UTF-8"),
        ({"data": {"a b": "c&d"}}, b"a+b=c%26d", "application/x-www-form-urlencoded;charset=UTF-8"),
        ({"json": {"a": [1, None]}}, b'{"a":[1,null]}', "application/json"),
    ],
    ids=["bytes", "text", "mapping", "json"],
)
def test_a_body_reaches_the_server_with_what_chrome_says_of_it(
    server: AnyMethodServer, options: BodyOptions, body: bytes, content_type: str | None
) -> None:
    with mitto.Session() as session:
        session.post(server.url, **options)

    received = server.received[0]
    assert received.body == body
    assert received.header("Content-Length") == str(len(body))
    assert received.header("Content-Type") == content_type
    assert received.header("Origin") == f"http://{server.authority}"


@pytest.mark.usefixtures("native_library")
def test_a_multipart_body_reaches_the_server_with_its_boundary(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.post(server.url, kind="navigation", data={"a": "b"}, files={"file": ("c.txt", b"d", "text/plain")})

    received = server.received[0]
    content_type = received.header("Content-Type")
    assert content_type is not None
    boundary = content_type.removeprefix("multipart/form-data; boundary=")
    assert (
        received.body
        == (
            f'--{boundary}\r\nContent-Disposition: form-data; name="a"\r\n\r\nb\r\n'
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="c.txt"\r\n'
            f"Content-Type: text/plain\r\n\r\nd\r\n--{boundary}--\r\n"
        ).encode()
    )


@pytest.mark.usefixtures("native_library")
def test_net_puts_a_content_length_after_connection_and_before_the_personas_lines(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        session.post(server.url, kind="navigation", data={"a": "b"})

    assert _lines_of(server.received[0])[:4] == ["Host", "Connection", "Content-Length", "Cache-Control"]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("name", "method"), [("post", "POST"), ("put", "PUT"), ("patch", "PATCH"), ("delete", "DELETE")]
)
def test_an_empty_body_is_a_body_for_any_method(server: AnyMethodServer, name: str, method: str) -> None:
    with mitto.Session() as session:
        getattr(session, name)(server.url, data=b"")

    received = server.received[0]
    assert (received.method, received.header("Content-Length"), received.body) == (method, "0", b"")


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("name", "content_length"), [("post", "0"), ("put", "0"), ("patch", None), ("delete", None), ("options", None)]
)
def test_with_no_body_net_says_a_length_of_0_for_post_and_put_only(
    server: AnyMethodServer, name: str, content_length: str | None
) -> None:
    with mitto.Session() as session:
        getattr(session, name)(server.url)

    assert server.received[0].header("Content-Length") == content_length


@pytest.mark.usefixtures("native_library")
def test_a_large_body_arrives_whole(server: AnyMethodServer) -> None:
    body = bytes(range(256)) * 8192

    with mitto.Session() as session:
        session.post(server.url, data=body)

    assert server.received[0].body == body


@pytest.mark.usefixtures("native_library")
def test_a_redirect_that_keeps_the_method_sends_the_body_again(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        response = session.post(f"{server.url}redirect-307", data="text")

    assert response.url == f"{server.url}final"
    assert [(request.method, request.path, request.body) for request in server.received] == [
        ("POST", "/redirect-307", b"text"),
        ("POST", "/final", b"text"),
    ]
    assert server.received[1].header("Content-Type") == "text/plain;charset=UTF-8"


@pytest.mark.usefixtures("native_library")
def test_a_redirect_that_turns_a_post_into_a_get_leaves_out_the_body_and_what_says_what_it_is(
    server: AnyMethodServer,
) -> None:
    with mitto.Session() as session:
        session.post(f"{server.url}redirect", data="text")

    final = server.received[1]
    assert (final.method, final.path, final.body) == ("GET", "/final", b"")
    assert {"Content-Length", "Content-Type", "Origin"}.isdisjoint(_lines_of(final))


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("name", ["get", "head"])
def test_a_body_with_get_or_head_is_refused_with_nothing_sent(server: AnyMethodServer, name: str) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="body"):
        getattr(session, name)(server.url, data="text")

    assert server.received == []


# An argument of each kind that test_request.py lists more of, in the type that code from `requests` gives it.
WRONG_TYPES = [
    ({"params": {"page": 2}}, "the value of 'page' in params has to be text, not int"),
    ({"params": {1: "a"}}, "a name in params has to be text, not int"),
    ({"params": [("page", None)]}, "the value of 'page' in params has to be text, not NoneType"),
    ({"params": "page=2"}, "params has to be a mapping or (name, value) pairs, not str"),
    ({"data": {"a": 1}}, "the value of 'a' in data has to be text, not int"),
    ({"data": {1: "a"}}, "a name in data has to be text, not int"),
    ({"data": {"a": 1}, "files": {"f": b""}}, "the value of 'a' in data has to be text, not int"),
    ({"files": {1: b""}}, "a name in files has to be text, not int"),
    ({"headers": {"X-A": 1}}, "the value of 'X-A' in headers has to be text or None, not int"),
    ({"headers": {1: "a"}}, "a name in headers has to be text, not int"),
    ({"headers": [("X-A", "1")]}, "headers has to be a mapping of names to text or None, not list"),
    ({"timeout": (3, 10)}, "timeout has to be a number of seconds or a Timeout, not tuple"),
]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("options", "message"), WRONG_TYPES)
def test_an_argument_of_the_wrong_type_is_named_with_nothing_sent(
    server: AnyMethodServer, options: dict[str, Any], message: str
) -> None:
    with mitto.Session() as session, pytest.raises(TypeError) as caught:
        session.post(server.url, **options)

    assert str(caught.value) == message
    assert server.received == []


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("options", "message"), WRONG_TYPES)
def test_a_stream_names_an_argument_of_the_wrong_type_with_nothing_sent(
    server: AnyMethodServer, options: dict[str, Any], message: str
) -> None:
    with mitto.Session() as session, pytest.raises(TypeError) as caught, session.stream("POST", server.url, **options):
        pytest.fail("the request started")

    assert str(caught.value) == message
    assert server.received == []


@pytest.mark.usefixtures("native_library")
def test_a_session_goes_on_after_an_argument_of_the_wrong_type(server: AnyMethodServer) -> None:
    with mitto.Session() as session:
        with pytest.raises(TypeError, match="params"):
            session.get(server.url, params={"page": 2})  # pyright: ignore[reportArgumentType]
        session.get(server.url, params={"page": "2"})

    assert [request.path for request in server.received] == ["/?page=2"]


@pytest.mark.usefixtures("native_library")
def test_get_names_an_argument_of_the_wrong_type(server: AnyMethodServer) -> None:
    with pytest.raises(TypeError, match="'page' in params"):
        mitto.get(server.url, params={"page": 2})  # pyright: ignore[reportArgumentType]

    assert server.received == []


class _BytesPath:
    """A path whose text is bytes, which pathlib does not take."""

    def __fspath__(self) -> bytes:
        return b"cookies.json"


@pytest.mark.parametrize(
    ("settings", "message"),
    [
        ({"persona": 5}, "persona has to be text, not int"),
        ({"persona": [ENGINE_RELEASE_PERSONA]}, "persona has to be text, not list"),
        ({"accept_language": 5}, "accept_language has to be text or None, not int"),
        ({"accept_language": ["de-DE"]}, "accept_language has to be text or None, not list"),
        (
            {"additional_trust_anchors": "one PEM"},
            "additional_trust_anchors has to be a sequence of PEM texts, not str",
        ),
        (
            {"additional_trust_anchors": b"one PEM"},
            "additional_trust_anchors has to be a sequence of PEM texts, not bytes",
        ),
        ({"additional_trust_anchors": 5}, "additional_trust_anchors has to be a sequence of PEM texts, not int"),
        ({"additional_trust_anchors": [b"one PEM"]}, "an item of additional_trust_anchors has to be text, not bytes"),
        (
            {"additional_trust_anchors": ("one PEM", None)},
            "an item of additional_trust_anchors has to be text, not NoneType",
        ),
        ({"cookie_file": 5}, "cookie_file has to be text, a path or None, not int"),
        ({"cookie_file": b"cookies.json"}, "cookie_file has to be text, a path or None, not bytes"),
        ({"cookie_file": _BytesPath()}, "cookie_file has to be text, a path or None, not _BytesPath"),
        ({"proxy": 5}, "proxy has to be text or None, not int"),
        ({"proxy": b"http://user:a-secret@proxy.test:8080"}, "proxy has to be text or None, not bytes"),
        ({"cache": 5}, "cache has to be text, a path or None, not int"),
        ({"cache": b"memory"}, "cache has to be text, a path or None, not bytes"),
    ],
)
def test_a_setting_of_the_wrong_type_is_named_before_the_library_loads(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, settings: dict[str, Any], message: str
) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(TypeError) as caught:
        mitto.Session(**settings)

    assert str(caught.value) == message


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("url_end", "options", "message"),
    [
        ("a-secret\0", {}, "url holds a NUL character"),
        ("a-secret\ud800", {}, "url holds a lone surrogate, which UTF-8 cannot encode"),
        ("", {"initiator": "https://a-secret.test/\0"}, "initiator holds a NUL character"),
        (
            "",
            {"initiator": "https://a-secret.test/\ud800"},
            "initiator holds a lone surrogate, which UTF-8 cannot encode",
        ),
        ("", {"headers": {"Referer": "https://a-secret.test/\0"}}, "the Referer in headers holds a NUL character"),
        (
            "",
            {"headers": {"referer": "https://a-secret.test/\udc00"}},
            "the Referer in headers holds a lone surrogate, which UTF-8 cannot encode",
        ),
    ],
)
def test_a_url_that_cannot_be_encoded_is_named_with_nothing_sent(
    server: AnyMethodServer, url_end: str, options: dict[str, Any], message: str
) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="holds") as caught:
        session.get(server.url + url_end, **options)

    assert str(caught.value) == message
    assert server.received == []


# The walk of every public argument. It reads them from the signatures, so a new argument is walked as soon as it exists,
# and fails here until it has a check.

# A value of the wrong type for each annotation that a public argument has, of a kind that code from `requests` might give.
WRONG_VALUES: dict[object, object] = {
    str: 5,
    str | None: 5,
    bool: "no",
    bool | None: "no",
    int | None: "443",
    float | None: "5",
    float | mitto.Timeout | None: (3, 10),
    Sequence[str]: b"-----BEGIN CERTIFICATE-----",
    str | os.PathLike[str]: 5,
    str | os.PathLike[str] | None: 5,
    QueryParams | None: "page=2",
    RequestHeaders | None: [("X-A", "1")],
    RequestKind | str | None: 5,
    RequestData | None: 5,
    RequestData | AsyncIterable[Buffer] | None: 5,
    FileFields | None: "report.csv",
    # Any value is an object, which is what json takes, but JSON has no form of a set.
    object: {"a"},
    datetime | None: 1_800_000_000,
    mitto.Cookie: {"a": "1"},
    mitto.CookiePartitionKey | None: "https://example.test",
    SameSite: 5,
    CookiePriority: 5,
    CookieSourceScheme | None: 5,
    CookieSourceType | None: 5,
    int: "8",
    float: "1",
    ColorScheme: 5,
    ReducedMotion: 5,
    ReducedTransparency: 5,
    ColorScheme | None: 5,
    ReducedMotion | None: 5,
    ReducedTransparency | None: 5,
    WindowState: 5,
    ContrastTheme: 5,
    InvertedColors: 5,
    WindowState | None: 5,
    ContrastTheme | None: 5,
    InvertedColors | None: 5,
    mitto.Machine | None: "mitto-0.1",
}

# The classes of each API, which the both_apis fixture puts behind mitto.Session.
API_CLASSES = {"sync": (mitto.Session, mitto.Cookies), "asyncio": (mitto.AsyncSession, mitto.AsyncCookies)}

# Each public name, from before the both_apis fixture puts another Session in place of mitto.Session.
PUBLIC = {name: getattr(mitto, name) for name in mitto.__all__}

_ERRORS = [name for name, item in PUBLIC.items() if inspect.isclass(item) and issubclass(item, Exception)]

# The public callables that take arguments and that the walk leaves out, each with the reason.
NOT_WALKED = {
    "Response()": "mitto builds what a response holds, and a caller never does",
    "ResponseStream()": "mitto builds what a response holds, and a caller never does",
    "AsyncResponseStream()": "mitto builds what a response holds, and a caller never does",
    "Redirect()": "mitto builds what a response holds, and a caller never does",
    "Headers()": "mitto builds what a response holds, and a caller never does",
    "Cookies()": "a Session makes its own, and a caller never does",
    "AsyncCookies()": "an AsyncSession makes its own, and a caller never does",
    "RequestKind()": "a value that is not a Request Kind raises a ValueError, as an Enum does, whatever its type",
    **dict.fromkeys((f"{name}()" for name in _ERRORS), "mitto raises its errors, and a caller never builds one"),
}


# What a Machine needs, which the wrong value of each takes the place of in turn.
MACHINE_VALUES: dict[str, object] = {
    name: value for name, value in dataclasses.asdict(mitto.Machine.named("mitto-0.1")).items() if name != "seed"
}


@dataclass(frozen=True, slots=True)
class Callee:
    """A public callable that takes arguments, and how the walk calls it.

    Attributes:
        name: Its name, as docs/api.md heads it.
        signature: What has its signature, whose arguments the walk reads.
        target: What gives the callable, from the Session that the walk makes.
        needs: A valid value for each argument that it needs, which the wrong one takes the place of in turn.
    """

    name: str
    signature: Callable[..., object]
    target: Callable[[Any], Callable[..., object]]
    needs: dict[str, object] = field(default_factory=dict[str, object])


def callees(api: str, url: str) -> list[Callee]:
    """Return each public callable that takes arguments, but those in NOT_WALKED, as the walk calls it through `api`."""
    session_class, cookies_class = API_CLASSES[api]
    walked = [
        Callee(f"{session_class.__name__}()", session_class, lambda _: mitto.Session),
        Callee("get()", mitto.get, lambda _: mitto.get, {"url": url}),
        Callee("Timeout()", mitto.Timeout, lambda _: mitto.Timeout),
        Callee("Cookie()", mitto.Cookie, lambda _: mitto.Cookie, {"name": "a", "value": "1", "domain": "example.test"}),
        Callee(
            "CookiePartitionKey()",
            mitto.CookiePartitionKey,
            lambda _: mitto.CookiePartitionKey,
            {"top_level_site": "https://example.test", "has_cross_site_ancestor": False},
        ),
        Callee("Machine()", mitto.Machine, lambda _: mitto.Machine, MACHINE_VALUES),
        Callee("Machine.draw()", mitto.Machine.draw, lambda _: mitto.Machine.draw),
        Callee("Machine.named()", mitto.Machine.named, lambda _: mitto.Machine.named),
        Callee("Headers.get()", mitto.Headers.get, lambda _: mitto.Headers().get, {"name": "a"}),
        Callee("Headers.get_all()", mitto.Headers.get_all, lambda _: mitto.Headers().get_all, {"name": "a"}),
    ]
    for name in ["request", "stream", *METHODS]:
        needs: dict[str, object] = {"method": "POST", "url": url} if name in {"request", "stream"} else {"url": url}
        walked.append(
            Callee(f"{session_class.__name__}.{name}()", getattr(session_class, name), operator.attrgetter(name), needs)
        )
    for name in ["get", "set", "delete", "save", "load"]:
        needs = {"name": "a"} if name in {"get", "delete"} else {}
        walked.append(
            Callee(
                f"{cookies_class.__name__}.{name}()",
                getattr(cookies_class, name),
                operator.attrgetter(f"cookies.{name}"),
                needs,
            )
        )
    return walked


def walked_arguments(signature: Callable[..., object]) -> Iterator[tuple[str, object]]:
    """Yield the name and annotation of each public argument, with the keys of an Unpack[TypedDict] as arguments."""
    for parameter in inspect.signature(signature).parameters.values():
        if parameter.name == "self" or parameter.name.startswith("_"):
            continue
        if parameter.kind is inspect.Parameter.VAR_KEYWORD:
            (options,) = typing.get_args(parameter.annotation)
            yield from typing.get_type_hints(options).items()
        else:
            yield parameter.name, parameter.annotation


def fault_of(call: Callable[[], object], argument: str, wrong: object) -> str | None:
    """Return what is wrong with how `call` takes `wrong` as `argument`, or None if it raises a TypeError as it should.

    That TypeError names the argument and the type it got, and leaves out the value. Something to enter is entered.
    """
    try:
        result: Any = call()
        if hasattr(result, "__enter__"):
            with result:
                pass
    except TypeError as error:
        message = str(error)
        if not re.search(rf"\b{argument}\b", message) or not re.search(rf"\b{type(wrong).__name__}\b", message):
            return f"the TypeError does not name it and its type: {message}"
        if repr(wrong) in message:
            return f"the TypeError shows the value: {message}"
        return None
    except Exception as error:
        return f"it raises {type(error).__name__}: {error}"
    return "it raises nothing"


@pytest.mark.usefixtures("native_library")
def test_each_public_argument_of_the_wrong_type_is_named_with_nothing_sent(
    both_apis: str, server: AnyMethodServer
) -> None:
    faults: list[str] = []
    with mitto.Session() as session:
        for callee in callees(both_apis, server.url):
            for argument, annotation in walked_arguments(callee.signature):
                if annotation not in WRONG_VALUES:
                    faults.append(f"{callee.name} {argument}=: WRONG_VALUES has nothing for {annotation}")
                    continue
                wrong = WRONG_VALUES[annotation]
                call = functools.partial(callee.target(session), **{**callee.needs, argument: wrong})
                if (fault := fault_of(call, argument, wrong)) is not None:
                    faults.append(f"{callee.name} {argument}=: {fault}")

    assert faults == []
    assert server.received == []


def _takes_arguments(item: Callable[..., object]) -> bool:
    try:
        parameters = inspect.signature(item).parameters
    except ValueError:
        # A builtin type such as Exception has no signature to read, and takes any arguments.
        return True
    return any(name != "self" and not name.startswith("_") for name in parameters)


def public_callables() -> set[str]:
    """Return each function, class and method in PUBLIC that takes arguments, named as callees() names them."""
    names: set[str] = set()
    for name, item in PUBLIC.items():
        if _takes_arguments(item):
            names.add(f"{name}()")
        if inspect.isclass(item):
            # A class method, such as Machine.draw(), is bound to its class already.
            names |= {
                f"{name}.{method_name}()"
                for method_name, method in inspect.getmembers(
                    item, lambda member: inspect.isfunction(member) or inspect.ismethod(member)
                )
                if not method_name.startswith("_") and _takes_arguments(method)
            }
    return names


def test_the_walk_reaches_each_public_callable_that_takes_arguments() -> None:
    walked = {callee.name for api in API_CLASSES for callee in callees(api, "")}

    assert sorted(public_callables() - walked - NOT_WALKED.keys()) == []
    assert sorted((walked | NOT_WALKED.keys()) - public_callables()) == []


def test_the_walk_finds_an_argument_with_no_check() -> None:
    def unchecked(text: object) -> None:
        pass

    def checked(text: object) -> None:
        if not isinstance(text, str):
            raise TypeError(f"text has to be text, not {type(text).__name__}")

    assert fault_of(functools.partial(unchecked, 5), "text", 5) == "it raises nothing"
    assert fault_of(functools.partial(checked, 5), "text", 5) is None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(("route", "protocol"), [(HTTP1, "http/1.1"), (HTTP2, "h2"), (HTTP3, "h3")])
def test_a_body_larger_than_one_frame_arrives_whole_over_each_protocol(route: str, protocol: str) -> None:
    body = bytes(range(256)) * 160
    with CaptureServer() as capture_server:
        settings = PrivateSettings(
            ignore_certificate_errors_spki_list=[capture_server.spki_hash],
            origins_to_force_quic_on=[capture_server.authority(HTTP3)] if route == HTTP3 else [],
        )
        with mitto.Session(_settings=settings) as session:
            path = KIND_PATHS[CaptureRequestKind.FETCH]
            response = session.post(capture_server.url(route, path=path), data=body)
        form = normal_form(capture_server.recording(route), MAIN_CASES)

    assert response.protocol == protocol
    request: Any = cast(dict[str, Any], form)["requests"]["fetch"]
    assert request["body"].encode("latin-1") == body
