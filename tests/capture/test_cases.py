"""Tests of the Recording Cases with no Chrome: what stands for each, how the server answers, and what the pages ask for."""

import contextlib
import json
import logging
import re
import socket
import ssl
import subprocess
import sys
import time
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Protocol
from urllib.parse import urlsplit

import pytest

from tests.capture.clients import Answer, Http2Client, client_context, connect, http1_exchange
from tests.capture.http3_client import Http3Client
from tests.local_server import unused_ports
from tools.capture import (
    CASE_HOST,
    CASES,
    CROSS_SITE_HOST,
    DOCUMENT_CASES,
    DOCUMENT_RUNS,
    HTTP1,
    HTTP2,
    HTTP3,
    MAIN_CASES,
    PLAIN_HTTP,
    PROXIED_HTTP1,
    PROXIED_HTTP2,
    PROXY,
    RECORDING_RUNS,
    RUNS,
    SAME_SITE_HOST,
    CaptureServer,
    Run,
    normal_form,
    raw_form,
)
from tools.capture.cases import HINTS_ASKED, HOST_NAMES, PROXY_CONNECT, START_PATH, respond_on
from tools.capture.page import KIND_PATHS, RequestKind
from tools.capture.recording import ConnectionRecord, Json
from tools.capture.script_page import SCRIPT_RUNS

_REPOSITORY = Path(__file__).parents[2]
_TIMEOUT_SECONDS = 10.0
_BOUNDARY = "----WebKitFormBoundaryAbCdEfGh01234567"
_RUNS_BY_NAME = {run.name: run for run in RUNS}
_RECORDING_RUNS_BY_NAME = {run.name: run for run in RECORDING_RUNS}
_DOCUMENT_ROUTES = {"documents/http1": HTTP1, "documents/http2": HTTP2, "documents/http3": HTTP3}
_PAGE_ORIGIN = f"https://{CASE_HOST}:8443"
_DOCUMENT_PATH = "/case/document"
_TLS_ROUTES = {"cases/http1": HTTP1, "cases/http2": HTTP2, "cases/http3": HTTP3}
_CLIENT_HINTS_ROUTES = {
    "cases/client_hints_http1": HTTP1,
    "cases/client_hints_http2": HTTP2,
    "cases/client_hints_http3": HTTP3,
}
_REQUEST_OF: dict[str, tuple[str, tuple[tuple[str, str], ...], bytes]] = {
    "navigation_form_urlencoded": ("POST", (("Content-Type", "application/x-www-form-urlencoded"),), b"a=b"),
    "navigation_form_multipart": (
        "POST",
        (("Content-Type", f"multipart/form-data; boundary={_BOUNDARY}"),),
        f'--{_BOUNDARY}\r\nContent-Disposition: form-data; name="a"\r\n\r\nb\r\n--{_BOUNDARY}--\r\n'.encode(),
    ),
    "fetch_post_text_same_origin": ("POST", (("Content-Type", "text/plain;charset=UTF-8"),), b"text body"),
    "fetch_post_text_cross_site": ("POST", (("Content-Type", "text/plain;charset=UTF-8"),), b"text body"),
    "fetch_post_json_same_origin": ("POST", (("Content-Type", "application/json"),), b'{"json":"body"}'),
    "fetch_post_stream": ("POST", (), b"stream body"),
    "cookie": ("GET", (("Cookie", "mitto=1"),), b""),
    "revalidation": ("GET", (("If-None-Match", '"mitto"'),), b""),
    "client_hints_critical": ("GET", (("sec-ch-ua-arch", '"x86"'),), b""),
    "preflight": (
        "OPTIONS",
        (("Access-Control-Request-Method", "POST"), ("Access-Control-Request-Headers", "content-type")),
        b"",
    ),
    "fetch_post_json_cross_site": ("POST", (("Content-Type", "application/json"),), b'{"json":"body"}'),
    "navigation_history_back": ("GET", (("Cookie", "mitto=1"),), b""),
}


class _Exchanger(Protocol):
    def exchange(
        self,
        method: str,
        target: str,
        *,
        authority: str,
        headers: Sequence[tuple[str, str]] = (),
        body: bytes = b"",
    ) -> Answer: ...


class _Http1Client:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock

    def exchange(
        self,
        method: str,
        target: str,
        *,
        authority: str,
        headers: Sequence[tuple[str, str]] = (),
        body: bytes = b"",
    ) -> Answer:
        return http1_exchange(self.sock, method, target, authority=authority, headers=headers, body=body)


def get(client: _Exchanger, authority: str, target: str, *headers: tuple[str, str]) -> Answer:
    return client.exchange("GET", target, authority=authority, headers=headers)


def open_tunnel(server: CaptureServer, target: str, *, head: bytes | None = None) -> tuple[socket.socket, bytes]:
    """Ask the proxy for a tunnel and return the socket with its answer."""
    sock = socket.create_connection(("127.0.0.1", server.port(PROXY)), timeout=_TIMEOUT_SECONDS)
    request = f"CONNECT {target} HTTP/1.1\r\nHost: {target}\r\nProxy-Connection: keep-alive\r\n\r\n".encode()
    answer = bytearray()
    # A server that gives up with data unread resets the connection, which can overtake what it said before.
    with contextlib.suppress(OSError):
        sock.sendall(request if head is None else head)
        while b"\r\n\r\n" not in answer:
            more = sock.recv(1 << 16)
            if not more:
                break
            answer += more
    return sock, bytes(answer)


def through_tunnel(server: CaptureServer, protocol: str, offered: tuple[str, ...]) -> ssl.SSLSocket:
    sock, answer = open_tunnel(server, server.authority(protocol, host=CASE_HOST))
    assert answer.startswith(b"HTTP/1.1 200 ")
    return client_context(offered).wrap_socket(sock, server_hostname=CASE_HOST)


def send_cases(client: _Exchanger, run: Run, authority: str) -> None:
    """Send the request of each of a run's cases, with what each has to carry, and then the path that ends it."""
    for case in run.cases:
        if case is PROXY_CONNECT:
            continue
        method, headers, body = _REQUEST_OF.get(case.name, ("GET", (), b""))
        answer = client.exchange(method, case.path, authority=authority, headers=headers, body=body)
        assert answer.status in {200, 302, 304}, case.name
    client.exchange("GET", run.done_path, authority=authority)


def requests_of(form: dict[str, Json]) -> dict[str, Json]:
    requests = form["requests"]
    assert isinstance(requests, dict)
    return requests


def request_of(form: dict[str, Json], name: str) -> dict[str, Json]:
    request = requests_of(form)[name]
    assert isinstance(request, dict)
    return request


def header_lines_of(form: dict[str, Json], name: str) -> list[str]:
    lines = request_of(form, name)["headers"]
    assert isinstance(lines, list)
    return [line for line in lines if isinstance(line, str)]


def test_every_case_has_one_name_and_one_path_and_one_run() -> None:
    names = [case.name for case in CASES]
    paths = [case.path for case in CASES]

    assert len(set(names)) == len(names)
    assert len(set(paths)) == len(paths)
    assert {case.name for run in RUNS for case in run.cases} == set(names)
    assert {run.name for run in RUNS} == set(_RUNS_BY_NAME)
    assert len(_RUNS_BY_NAME) == len(RUNS)


def test_the_main_runs_hold_the_main_request_kinds_in_their_order() -> None:
    assert [case.name for case in MAIN_CASES] == ["navigation", "fetch", "script", "image"]
    assert [case.path for case in MAIN_CASES] == [KIND_PATHS[kind] for kind in RequestKind]
    for name in ("http1", "http2", "http3"):
        assert _RUNS_BY_NAME[name].cases == MAIN_CASES


@pytest.mark.parametrize(("name", "protocol"), _TLS_ROUTES.items())
def test_a_client_can_produce_every_case_of_a_run_directly(name: str, protocol: str) -> None:
    run = _RUNS_BY_NAME[name]
    with CaptureServer() as server:
        authority = server.authority(protocol, host=CASE_HOST)
        if protocol == HTTP3:
            with Http3Client(server.port(HTTP3), host=CASE_HOST) as client:
                send_cases(client, run, authority)
        else:
            offered = ("h2",) if protocol == HTTP2 else ("http/1.1",)
            with connect(server.port(protocol), offered=offered, host=CASE_HOST) as sock:
                client = Http2Client(sock) if protocol == HTTP2 else _Http1Client(sock)
                send_cases(client, run, authority)
        assert server.wait_for_page(protocol, _TIMEOUT_SECONDS, done_path=run.done_path)
        form = normal_form(server.recording(protocol, host=CASE_HOST), run.cases)

    assert form["protocol"] == protocol
    assert list(requests_of(form)) == [case.name for case in run.cases]
    for case_name, (_, _, body) in _REQUEST_OF.items():
        if body and b"--" not in body and case_name in requests_of(form):
            assert request_of(form, case_name)["body"] == body.decode()
    assert header_lines_of(form, "cookie")[-1].lower() == "cookie: mitto=1"
    assert header_lines_of(form, "revalidation")[-1].lower() == 'if-none-match: "mitto"'


@pytest.mark.parametrize(("name", "protocol"), _CLIENT_HINTS_ROUTES.items())
def test_a_client_can_produce_every_client_hints_case_of_a_run_directly(name: str, protocol: str) -> None:
    run = _RUNS_BY_NAME[name]
    with CaptureServer() as server:
        authority = server.authority(protocol, host=CASE_HOST)
        if protocol == HTTP3:
            with Http3Client(server.port(HTTP3), host=CASE_HOST) as client:
                send_cases(client, run, authority)
        else:
            offered = ("h2",) if protocol == HTTP2 else ("http/1.1",)
            with connect(server.port(protocol), offered=offered, host=CASE_HOST) as sock:
                send_cases(Http2Client(sock) if protocol == HTTP2 else _Http1Client(sock), run, authority)
        assert server.wait_for_page(protocol, _TIMEOUT_SECONDS, done_path=run.done_path)
        form = normal_form(server.recording(protocol, host=CASE_HOST), run.cases)

    assert list(requests_of(form)) == [case.name for case in run.cases]


def _answer_to(path: str, scheme: str = "https", port: int = 8443) -> dict[str, str]:
    class Head:
        target = path
        headers: tuple[tuple[str, str], ...] = ()

    return dict(respond_on(scheme, port, Head()).headers)


def test_the_first_page_of_the_client_hints_runs_asks_for_every_hint() -> None:
    asked = _answer_to("/case/client-hints-first")["Accept-CH"].split(", ")

    assert len(asked) == len(set(asked)) == 27
    assert {"sec-ch-ua-full-version-list", "device-memory", "save-data", "sec-ch-width"} <= set(asked)


@pytest.mark.parametrize("kind", ["script", "image", "fetch"])
@pytest.mark.parametrize("where", ["same-site", "cross-site"])
def test_a_subresource_of_another_origin_asks_too_which_shows_that_it_counts_for_nothing(kind: str, where: str) -> None:
    assert "Accept-CH" in _answer_to(f"/case/client-hints-{kind}-{where}")


def test_each_set_of_hints_is_asked_for_by_its_own_page_and_only_there() -> None:
    assert [_answer_to(asked.path)["Accept-CH"] for asked in HINTS_ASKED] == [asked.accept_ch for asked in HINTS_ASKED]
    for asked in HINTS_ASKED:
        assert "Accept-CH" not in _answer_to(asked.fetch.path)
        assert "Accept-CH" not in _answer_to(asked.navigation.path)


def test_the_page_that_takes_back_what_was_asked_for_sends_an_empty_header() -> None:
    assert _answer_to(HINTS_ASKED[-1].path)["Accept-CH"] == ""


def test_the_last_page_of_the_client_hints_runs_asks_for_a_critical_hint() -> None:
    answer = _answer_to("/case/client-hints-critical")

    assert answer["Accept-CH"] == answer["Critical-CH"] == "sec-ch-ua-arch"


def test_a_redirect_asks_for_every_hint_at_the_other_origin_of_the_site() -> None:
    assert "Accept-CH" in _answer_to("/case/client-hints-redirect-navigation-2")
    assert "Accept-CH" not in _answer_to("/case/client-hints-redirect-navigation-1")


def test_a_client_can_produce_every_case_over_plain_http() -> None:
    run = _RUNS_BY_NAME["cases/plain_http"]
    with CaptureServer() as server:
        authority = server.authority(PLAIN_HTTP, host=CASE_HOST)
        with socket.create_connection(("127.0.0.1", server.port(PLAIN_HTTP)), timeout=_TIMEOUT_SECONDS) as sock:
            send_cases(_Http1Client(sock), run, authority)
        assert server.wait_for_page(PLAIN_HTTP, _TIMEOUT_SECONDS, done_path=run.done_path)
        recording = server.recording(PLAIN_HTTP, host=CASE_HOST)

    form = normal_form(recording, run.cases)
    assert list(form) == ["format", "protocol", "requests"]
    assert form["protocol"] == "http/1.1"
    assert list(requests_of(form)) == [case.name for case in run.cases]
    assert recording.scheme == "http"
    connection = recording.connections[0]
    assert isinstance(connection, ConnectionRecord)
    assert connection.client_hello is None
    assert connection.tls_version is None
    raw = raw_form(recording)
    assert isinstance(raw, dict)
    assert json.dumps(raw)


@pytest.mark.parametrize(
    ("name", "protocol", "route", "offered"),
    [
        ("cases/proxy_http1", HTTP1, PROXIED_HTTP1, ("http/1.1",)),
        ("cases/proxy_http2", HTTP2, PROXIED_HTTP2, ("h2",)),
    ],
)
def test_a_client_can_produce_every_case_through_the_proxy(
    name: str, protocol: str, route: str, offered: tuple[str, ...]
) -> None:
    run = _RUNS_BY_NAME[name]
    with CaptureServer() as server:
        authority = server.authority(route, host=CASE_HOST)
        with through_tunnel(server, protocol, offered) as sock:
            client = Http2Client(sock) if protocol == HTTP2 else _Http1Client(sock)
            send_cases(client, run, authority)
        assert server.wait_for_page(route, _TIMEOUT_SECONDS, done_path=run.done_path)
        assert not server.wait_for_page(protocol, 0, done_path=run.done_path)
        assert server.recording(protocol).connections == ()
        form = normal_form(server.recording(route, host=CASE_HOST), run.cases)
        raw = raw_form(server.recording(route, host=CASE_HOST))

    assert form["protocol"] == protocol
    assert list(requests_of(form)) == ["proxy_connect", "proxy_navigation"]
    assert request_of(form, "proxy_connect") == {
        "request_line": "CONNECT <authority> HTTP/1.1",
        "headers": ["Host: <authority>", "Proxy-Connection: keep-alive"],
    }
    assert isinstance(raw, dict)
    connections = raw["connections"]
    assert isinstance(connections, list)
    first = connections[0]
    assert isinstance(first, dict)
    connect_form = first["connect"]
    assert isinstance(connect_form, dict)
    assert str(connect_form["request_line"]).startswith(f"CONNECT {authority} ")


@pytest.mark.parametrize("target", ["example.com:443", f"{CASE_HOST}:1", CASE_HOST])
def test_the_proxy_refuses_a_tunnel_to_anywhere_else(target: str) -> None:
    with CaptureServer() as server:
        sock, answer = open_tunnel(server, target)
        with sock:
            assert answer.startswith(b"HTTP/1.1 403 ")
            assert sock.recv(1) == b""
        assert server.recording(PROXIED_HTTP1).connections == ()


def test_the_proxy_refuses_a_tunnel_to_a_plain_port_and_anything_but_connect() -> None:
    with CaptureServer() as server:
        plain_sock, plain_answer = open_tunnel(server, server.authority(PLAIN_HTTP, host=CASE_HOST))
        get_sock, get_answer = open_tunnel(
            server, "", head=f"GET http://{CASE_HOST}/ HTTP/1.1\r\nHost: {CASE_HOST}\r\n\r\n".encode()
        )
        with plain_sock, get_sock:
            assert plain_answer.startswith(b"HTTP/1.1 403 ")
            assert get_answer.startswith(b"HTTP/1.1 403 ")


def test_the_proxy_ends_a_connection_that_does_not_wait_for_its_answer(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING), CaptureServer() as server:
        target = server.authority(HTTP1, host=CASE_HOST)
        sock, answer = open_tunnel(server, "", head=f"CONNECT {target} HTTP/1.1\r\n\r\nearly".encode())
        with sock:
            assert answer == b""

    assert "did not wait" in caplog.text


@pytest.mark.parametrize("name", HOST_NAMES)
def test_the_proxy_opens_a_tunnel_to_each_name(name: str) -> None:
    with CaptureServer() as server:
        sock, answer = open_tunnel(server, f"{name}:{server.port(HTTP1)}")
        with sock:
            assert answer == b"HTTP/1.1 200 Connection established\r\n\r\n"


def test_the_proxy_survives_a_client_that_goes_away_or_makes_no_sense(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING), CaptureServer() as server:
        with socket.create_connection(("127.0.0.1", server.port(PROXY)), timeout=_TIMEOUT_SECONDS):
            pass
        sock, answer = open_tunnel(server, "", head=b"no request line\r\n\r\n")
        with sock:
            assert answer == b""
        sock, answer = open_tunnel(server, "", head=b"a" * (1 << 17))
        with sock:
            assert answer == b""
        with through_tunnel(server, HTTP1, ("http/1.1",)) as tunnel:
            assert get(_Http1Client(tunnel), "x", "/fetch").status == 200


def test_a_case_counts_only_with_what_it_has_to_carry() -> None:
    run = _RUNS_BY_NAME["cases/http1"]
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        client = _Http1Client(sock)
        authority = server.authority(HTTP1, host=CASE_HOST)
        first = get(client, authority, "/case/revalidation")
        get(client, authority, "/case/cookie")
        get(client, authority, "/case/fetch-get-same-site")
        form = normal_form(server.recording(HTTP1, host=CASE_HOST), run.cases)
        assert "revalidation" not in requests_of(form)
        assert "cookie" not in requests_of(form)

        second = get(client, authority, "/case/revalidation", ("If-None-Match", first.headers["etag"]))
        form = normal_form(server.recording(HTTP1, host=CASE_HOST), run.cases)

    assert "revalidation" in requests_of(form)
    assert first.status == 200
    assert first.headers["cache-control"] == "no-cache"
    assert second.status == 304
    assert second.headers["etag"] == first.headers["etag"]
    assert second.body == b""


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_cookie_is_set_only_for_the_path_that_shows_it(protocol: str) -> None:
    with CaptureServer() as server:
        authority = server.authority(protocol, host=CASE_HOST)
        if protocol == HTTP3:
            with Http3Client(server.port(HTTP3), host=CASE_HOST) as client:
                answer = get(client, authority, "/case/cookie-set")
        else:
            with connect(server.port(protocol), offered=(protocol,), host=CASE_HOST) as sock:
                client = Http2Client(sock) if protocol == HTTP2 else _Http1Client(sock)
                answer = get(client, authority, "/case/cookie-set")

    assert answer.headers["set-cookie"] == "mitto=1; Path=/case/cookie"


@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_a_response_without_a_body_ends_with_its_headers(protocol: str) -> None:
    """Chrome resets a stream that goes on after a 304, which would put a frame in the normal form."""
    with CaptureServer() as server:
        authority = server.authority(protocol, host=CASE_HOST)
        if protocol == HTTP3:
            with Http3Client(server.port(HTTP3), host=CASE_HOST) as client:
                answer = get(client, authority, "/case/revalidation", ("If-None-Match", '"mitto"'))
        else:
            with connect(server.port(protocol), offered=(protocol,), host=CASE_HOST) as sock:
                answer = get(Http2Client(sock), authority, "/case/revalidation", ("If-None-Match", '"mitto"'))

    assert answer.status == 304
    assert answer.body == b""


@pytest.mark.parametrize(
    ("start", "hosts"),
    [
        ("/case/redirect-navigation-1", (SAME_SITE_HOST, CROSS_SITE_HOST, CASE_HOST)),
        ("/case/redirect-fetch-1", (SAME_SITE_HOST, CROSS_SITE_HOST, CASE_HOST)),
    ],
)
def test_redirect_chains_cross_origins_and_sites_and_allow_every_origin(start: str, hosts: tuple[str, ...]) -> None:
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        client = _Http1Client(sock)
        port = server.port(HTTP1)
        answer = get(client, f"{CASE_HOST}:{port}", start)
        assert answer.headers["access-control-allow-origin"] == "*"
        for number, host in enumerate(hosts, start=2):
            assert answer.status == 302
            expected = f"https://{host}:{port}{start[:-1]}{number}"
            assert answer.headers["location"] == expected
            answer = get(client, f"{host}:{port}", expected.removeprefix(f"https://{host}:{port}"))
            assert answer.headers["access-control-allow-origin"] == "*"
        assert answer.status == 200


@pytest.mark.parametrize("kind", list(RequestKind))
def test_the_main_paths_send_nothing_new(kind: RequestKind) -> None:
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",)) as sock:
        answer = get(_Http1Client(sock), server.authority(HTTP1), KIND_PATHS[kind])

    assert set(answer.headers) == {"content-type", "content-length", "cache-control", "connection"}
    assert answer.headers["cache-control"] == "no-store"


def _crawl(client: _Exchanger, port: int, start: str) -> set[str]:
    """Return every case path that a page or redirect names, reached by following them from start."""
    seen: set[str] = set()
    queue = [start]
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        answer = get(client, f"{CASE_HOST}:{port}", path)
        text = answer.body.decode("latin-1") + answer.headers.get("location", "")
        queue += re.findall(r"/case/[a-z0-9-]+", text)
    return seen


@pytest.mark.parametrize("name", [run.name for run in RUNS if run.host is not None])
def test_the_pages_of_a_run_ask_for_every_case_of_it(name: str) -> None:
    run = _RUNS_BY_NAME[name]
    with CaptureServer() as server:
        # What a page asks for does not depend on the protocol that brings it, so one is enough here.
        if run.route == PLAIN_HTTP:
            with socket.create_connection(("127.0.0.1", server.port(PLAIN_HTTP)), timeout=_TIMEOUT_SECONDS) as sock:
                reached = _crawl(_Http1Client(sock), server.port(PLAIN_HTTP), run.start_path)
        else:
            with connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
                reached = _crawl(_Http1Client(sock), server.port(HTTP1), run.start_path)

    wanted = {case.path for case in run.cases if case is not PROXY_CONNECT}
    assert wanted <= reached
    assert run.done_path in reached


def test_the_start_page_names_each_origin_with_the_port_it_is_served_on() -> None:
    with CaptureServer() as server, connect(server.port(HTTP2), offered=("http/1.1",), host=CASE_HOST) as sock:
        port = server.port(HTTP2)
        page = get(_Http1Client(sock), f"{CASE_HOST}:{port}", START_PATH).body.decode()

    assert f'<script src="https://{SAME_SITE_HOST}:{port}/case/script-same-site">' in page
    assert f'<script src="https://{CROSS_SITE_HOST}:{port}/case/script-cross-site">' in page
    assert f'<img src="https://{SAME_SITE_HOST}:{port}/case/image-same-site"' in page
    assert f'<img src="https://{CROSS_SITE_HOST}:{port}/case/image-cross-site"' in page


@pytest.mark.parametrize(
    ("path", "enctype"),
    [("/case/forms", 'action="/case/navigation-form-urlencoded"'), ("/case/navigation-form-urlencoded", "multipart")],
)
def test_the_forms_post_navigations(path: str, enctype: str) -> None:
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        client = _Http1Client(sock)
        page = client.exchange("GET", path, authority=f"{CASE_HOST}:{server.port(HTTP1)}").body.decode()

    assert 'method="post"' in page
    assert enctype in page
    assert 'id="next"' in page


def test_what_went_wrong_in_a_page_reaches_the_log(caplog: pytest.LogCaptureFixture) -> None:
    with (
        caplog.at_level(logging.WARNING),
        CaptureServer() as server,
        connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock,
    ):
        get(_Http1Client(sock), f"{CASE_HOST}:{server.port(HTTP1)}", "/case/failed?TypeError%3A%20no")

    assert "the page failed: TypeError: no" in caplog.text


def test_names_show_as_placeholders_and_what_a_client_randomizes_does_not_show() -> None:
    run = _RUNS_BY_NAME["cases/http2"]
    with CaptureServer() as server, connect(server.port(HTTP2), offered=("h2",), host=CASE_HOST) as sock:
        port = server.port(HTTP2)
        client = Http2Client(sock)
        client.exchange(
            "POST",
            "/case/fetch-post-text-cross-site",
            authority=f"{CROSS_SITE_HOST}:{port}",
            headers=(
                ("Origin", f"https://{CASE_HOST}:{port}"),
                ("Referer", f"https://{SAME_SITE_HOST}:{port}/page"),
                ("X-Other", f"https://{CROSS_SITE_HOST}:{port} {SAME_SITE_HOST}:{port} {CASE_HOST}:{port}"),
            ),
            body=b"a",
        )
        _, headers, body = _REQUEST_OF["navigation_form_multipart"]
        client.exchange(
            "POST", "/case/navigation-form-multipart", authority=f"{CASE_HOST}:{port}", headers=headers, body=body
        )
        form = normal_form(server.recording(HTTP2, host=CASE_HOST), run.cases)

    lines = header_lines_of(form, "fetch_post_text_cross_site")
    assert ":authority: <cross-site-authority>" in lines
    assert "origin: <origin>" in lines
    assert "referer: <same-site-origin>/page" in lines
    assert "x-other: <cross-site-origin> <same-site-authority> <authority>" in lines
    multipart = request_of(form, "navigation_form_multipart")
    assert "content-type: multipart/form-data; boundary=----WebKitFormBoundary<random>" in header_lines_of(
        form, "navigation_form_multipart"
    )
    assert multipart["body"] == (
        '------WebKitFormBoundary<random>\r\nContent-Disposition: form-data; name="a"\r\n\r\nb\r\n'
        "------WebKitFormBoundary<random>--\r\n"
    )


def test_a_boundary_a_client_did_not_randomize_the_way_chrome_does_stays_as_sent() -> None:
    run = _RUNS_BY_NAME["cases/http1"]
    boundary = "----WebKitFormBoundaryTooShort"
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        _Http1Client(sock).exchange(
            "POST",
            "/case/navigation-form-multipart",
            authority=f"{CASE_HOST}:{server.port(HTTP1)}",
            headers=(("Content-Type", f"multipart/form-data; boundary={boundary}"),),
            body=boundary.encode(),
        )
        form = normal_form(server.recording(HTTP1, host=CASE_HOST), run.cases)

    assert request_of(form, "navigation_form_multipart")["body"] == boundary


def test_a_body_keeps_every_byte() -> None:
    run = _RUNS_BY_NAME["cases/http1"]
    body = bytes(range(256))
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        _Http1Client(sock).exchange(
            "POST", "/case/fetch-post-stream", authority=f"{CASE_HOST}:{server.port(HTTP1)}", body=body
        )
        form = normal_form(server.recording(HTTP1, host=CASE_HOST), run.cases)
        raw = raw_form(server.recording(HTTP1, host=CASE_HOST))

    written = request_of(form, "fetch_post_stream")["body"]
    assert isinstance(written, str)
    assert written.encode("latin-1") == body
    assert json.loads(json.dumps(written)) == written
    assert body.hex() in json.dumps(raw)


def test_the_page_saying_a_request_was_refused_shows_in_its_place() -> None:
    run = _RUNS_BY_NAME["cases/http1"]
    with CaptureServer() as server, connect(server.port(HTTP1), offered=("http/1.1",), host=CASE_HOST) as sock:
        authority = f"{CASE_HOST}:{server.port(HTTP1)}"
        get(_Http1Client(sock), authority, "/case/fetch-post-stream-refused")
        get(_Http1Client(sock), authority, "/case/fetch-get-same-site")
        form = normal_form(server.recording(HTTP1, host=CASE_HOST), run.cases)

    assert request_of(form, "fetch_post_stream") == {"refused": True}


def _flags_of(flags: Iterable[str]) -> dict[str, str]:
    return {name: value for name, _, value in (flag.partition("=") for flag in flags)}


def test_each_run_starts_where_it_should_with_the_flags_it_needs() -> None:
    with CaptureServer() as server:
        spki = f"{server.spki_hash}"
        rules = f"MAP {CASE_HOST} 127.0.0.1,MAP {SAME_SITE_HOST} 127.0.0.1,MAP {CROSS_SITE_HOST} 127.0.0.1"
        flags = {run.name: _flags_of(server.chrome_flags(run)) for run in RUNS}
        urls = {run.name: server.start_url(run) for run in RUNS}
        http1, http2, http3 = server.port(HTTP1), server.port(HTTP2), server.port(HTTP3)
        plain = server.port(PLAIN_HTTP)
        proxy_url = server.proxy_url
        main_http3_authority = server.authority(HTTP3)

    assert all(value["--ignore-certificate-errors-spki-list"] == spki for value in flags.values())
    assert flags["http1"].keys() == {"--ignore-certificate-errors-spki-list"}
    assert flags["http3"]["--origin-to-force-quic-on"] == main_http3_authority
    assert urls["http1"] == f"https://capture.localhost:{http1}/"
    assert urls["cases/http2"] == f"https://{CASE_HOST}:{http2}/case/start"
    assert urls["cases/plain_http"] == f"http://{CASE_HOST}:{plain}/case/plain-http-navigation"
    assert urls["cases/proxy_http1"] == f"https://{CASE_HOST}:{http1}/case/proxy-navigation"
    assert urls["cases/proxy_http2"] == f"https://{CASE_HOST}:{http2}/case/proxy-navigation"
    for name in ("cases/http1", "cases/http2", "cases/http3", "cases/plain_http"):
        assert flags[name]["--host-resolver-rules"] == rules
        assert "--no-proxy-server" in flags[name]
        assert "--proxy-server" not in flags[name]
    assert flags["cases/http3"]["--origin-to-force-quic-on"] == ",".join(f"{name}:{http3}" for name in HOST_NAMES)
    for name in ("cases/proxy_http1", "cases/proxy_http2"):
        assert flags[name]["--proxy-server"] == proxy_url
        assert "--host-resolver-rules" not in flags[name]
        assert "--no-proxy-server" not in flags[name]


def test_the_server_can_be_at_a_test_name() -> None:
    with CaptureServer(CASE_HOST) as server:
        assert server.url(HTTP1) == f"https://{CASE_HOST}:{server.port(HTTP1)}/"
        assert server.recording(HTTP1).host == CASE_HOST


def test_a_named_plain_http_and_proxy_port_serve_there() -> None:
    plain, proxy = unused_ports(2)
    with CaptureServer(plain_http_port=plain, proxy_port=proxy) as server:
        assert server.port(PLAIN_HTTP) == plain
        assert server.port(PROXY) == proxy
        assert server.proxy_url == f"http://127.0.0.1:{proxy}"


def free_tcp_ports(count: int) -> list[int]:
    return unused_ports(count)


def _go_through_run(entry: dict[str, str], proxy_port: int) -> None:
    """Do with no Chrome what a run asks of one, through the address and route that the command line gave for it."""
    run = _RECORDING_RUNS_BY_NAME[entry["name"]]
    address = urlsplit(entry["url"])
    host, port = str(address.hostname), int(address.port or 0)
    authority = f"{host}:{port}"
    if run.route == HTTP3:
        with Http3Client(port, host=host) as client:
            send_cases(client, run, authority)
    elif run.route == PLAIN_HTTP:
        with socket.create_connection(("127.0.0.1", port), timeout=_TIMEOUT_SECONDS) as sock:
            send_cases(_Http1Client(sock), run, authority)
    elif run.route in {PROXIED_HTTP1, PROXIED_HTTP2}:
        offered = ("h2",) if run.route == PROXIED_HTTP2 else ("http/1.1",)
        raw = socket.create_connection(("127.0.0.1", proxy_port), timeout=_TIMEOUT_SECONDS)
        raw.sendall(f"CONNECT {authority} HTTP/1.1\r\nHost: {authority}\r\n\r\n".encode())
        assert raw.recv(1 << 16).startswith(b"HTTP/1.1 200 ")
        with client_context(offered).wrap_socket(raw, server_hostname=host) as sock:
            send_cases(Http2Client(sock) if run.route == PROXIED_HTTP2 else _Http1Client(sock), run, authority)
    else:
        offered = ("h2",) if run.route == HTTP2 else ("http/1.1",)
        with connect(port, offered=offered, host=host) as sock:
            send_cases(Http2Client(sock) if run.route == HTTP2 else _Http1Client(sock), run, authority)


def _post_a_record(entry: dict[str, str]) -> None:
    """Do what the page or worker of a Script Fingerprint does once the probe has finished: post a record."""
    (run,) = [run for run in SCRIPT_RUNS if run.name == entry["name"]]
    address = urlsplit(entry["url"])
    assert address.hostname is not None
    assert address.port is not None
    record = b'{"format": 1, "objects": [], "values": {}, "reports": []}'
    with connect(address.port, offered=("http/1.1",), host=address.hostname) as sock:
        http1_exchange(sock, "POST", run.done_path, authority=address.netloc, body=record)


def test_command_line_goes_through_every_run_and_says_how(tmp_path: Path) -> None:
    http1_port, http2_port, plain_http_port, proxy_port, script_port = free_tcp_ports(5)
    http3_port = unused_ports(1, socket.SOCK_DGRAM)[0]
    command = [
        sys.executable,
        "-m",
        "tools.capture",
        "--out",
        str(tmp_path),
        "--http1-port",
        str(http1_port),
        "--http2-port",
        str(http2_port),
        "--http3-port",
        str(http3_port),
        "--plain-http-port",
        str(plain_http_port),
        "--proxy-port",
        str(proxy_port),
        "--script-port",
        str(script_port),
        "--click-in",
        str(tmp_path / "no chrome here"),
    ]
    with subprocess.Popen(
        command, cwd=_REPOSITORY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    ) as process:
        runs_file = tmp_path / "runs.json"
        deadline = time.monotonic() + 30.0
        while not runs_file.exists():
            assert time.monotonic() < deadline, "no list of runs"
            assert process.poll() is None
            time.sleep(0.01)
        time.sleep(0.05)
        entries = json.loads(runs_file.read_text(encoding="utf-8"))["runs"]
        for entry in entries:
            if entry["fingerprint"] == "script":
                _post_a_record(entry)
            else:
                _go_through_run(entry, proxy_port)
        output = process.communicate(timeout=30.0)[0]
        assert process.returncode == 0, output

    # The Script Fingerprint first, from a user data directory that has only had its seed.
    assert [entry["name"] for entry in entries] == [run.name for run in (*SCRIPT_RUNS, *RECORDING_RUNS)]
    assert [entry["fingerprint"] for entry in entries] == ["script"] * len(SCRIPT_RUNS) + ["wire"] * len(RECORDING_RUNS)
    assert all(entry["flags"] for entry in entries)
    assert all(entry["label"] for entry in entries)
    assert '--host-resolver-rules="MAP capture.test 127.0.0.1,' in output
    for run in RECORDING_RUNS:
        form = json.loads((tmp_path / f"{run.name}.json").read_text(encoding="utf-8"))
        assert list(form["requests"]) == [case.name for case in run.cases]
        peers = {
            connection["peer"]
            for connection in json.loads((tmp_path / f"{run.name}.raw.json").read_text(encoding="utf-8"))["connections"]
        }
        assert peers == {"127.0.0.1"}


def test_each_document_case_has_a_name_and_a_path_of_its_own_but_the_preflight() -> None:
    names = [case.name for case in (*CASES, *DOCUMENT_CASES)]
    paths = [case.path for case in (*CASES, *DOCUMENT_CASES) if case.name != "preflight"]

    assert len(set(names)) == len(names)
    assert len(set(paths)) == len(paths)
    preflight, request = (case for case in DOCUMENT_CASES if case.path == "/case/fetch-post-json-cross-site")
    assert (preflight.name, preflight.carries) == ("preflight", "access-control-request-method")
    assert (request.name, request.carries) == ("fetch_post_json_cross_site", "content-type")
    for run in DOCUMENT_RUNS:
        assert run.cases == DOCUMENT_CASES
        assert run.name.startswith("documents/")
        assert set(run.ordered) < set(run.cases)


def test_a_recording_session_takes_the_document_runs_before_those_whose_hints_last() -> None:
    names = [run.name for run in RECORDING_RUNS]

    assert [run for run in RECORDING_RUNS if run not in DOCUMENT_RUNS] == list(RUNS)
    assert max(names.index(run.name) for run in DOCUMENT_RUNS) < names.index("cases/client_hints_http1")
    assert names.index("cases/proxy_http2") < min(names.index(run.name) for run in DOCUMENT_RUNS)


@pytest.mark.parametrize(("name", "protocol"), _DOCUMENT_ROUTES.items())
def test_a_client_can_produce_every_document_case_of_a_run_directly(name: str, protocol: str) -> None:
    run = _RECORDING_RUNS_BY_NAME[name]
    with CaptureServer() as server:
        authority = server.authority(protocol, host=CASE_HOST)
        if protocol == HTTP3:
            with Http3Client(server.port(HTTP3), host=CASE_HOST) as client:
                send_cases(client, run, authority)
        else:
            offered = ("h2",) if protocol == HTTP2 else ("http/1.1",)
            with connect(server.port(protocol), offered=offered, host=CASE_HOST) as sock:
                send_cases(Http2Client(sock) if protocol == HTTP2 else _Http1Client(sock), run, authority)
        assert server.wait_for_page(protocol, _TIMEOUT_SECONDS, done_path=run.done_path)
        form = normal_form(server.recording(protocol, host=CASE_HOST), run.cases, run.ordered)

    assert list(requests_of(form)) == [case.name for case in run.cases]
    assert form.get("order") == (None if protocol == HTTP1 else [case.name for case in run.ordered])
    assert request_of(form, "fetch_post_json_cross_site")["body"] == '{"json":"body"}'
    assert "body" not in request_of(form, "preflight")


class _Head:
    def __init__(self, target: str) -> None:
        self.target = target
        self.headers: tuple[tuple[str, str], ...] = ()


def _document_pages() -> dict[str, bytes]:
    """Return the body of each page and file that the pages of a document run name, from its first page on.

    A form that GETs sends its fields in the query, so its action gets them here too.
    """
    form_get = next(case.path for case in DOCUMENT_CASES if case.name == "navigation_form_get")
    submitted = {form_get.partition("?")[0]: form_get}
    bodies: dict[str, bytes] = {}
    queue = [DOCUMENT_RUNS[0].start_path]
    while queue:
        path = queue.pop()
        # The pages report there what went wrong.
        if path in bodies or path == "/case/failed":
            continue
        answer = respond_on("https", 8443, _Head(submitted.get(path, path)))
        assert answer.status in {200, 302}, path
        bodies[path] = answer.body
        text = answer.body.decode("latin-1") + dict(answer.headers).get("Location", "")
        queue += re.findall(r"/case/[a-z0-9-]+", text)
    return bodies


def test_the_pages_of_a_document_run_ask_for_every_case_of_it_or_chrome_does() -> None:
    pages = _document_pages()

    run = DOCUMENT_RUNS[0]
    reached = set(pages)
    assert run.done_path in reached
    for case in run.cases:
        if case.name == "favicon":
            # Chrome asks for it by itself, and the page says when it waited long enough.
            assert case.path not in "".join(page.decode("latin-1") for page in pages.values())
            assert case.refused_path in reached
        elif case.name == "navigation_form_get":
            path, _, query = case.path.partition("?")
            assert path in reached
            assert 'method="get"' in pages["/case/documents-forms"].decode()
            assert query == "name=mitto&a+b%26c=d+e%26f%2Bg"
        else:
            assert case.path in reached, case.name


def test_only_the_document_names_no_icon_so_chrome_asks_for_one_there_alone() -> None:
    pages = _document_pages()

    documents = {path: page.decode() for path, page in pages.items() if page.startswith(b"<!doctype html>")}
    assert _DOCUMENT_PATH in documents
    assert [path for path, page in documents.items() if 'rel="icon"' not in page] == [_DOCUMENT_PATH]


def test_the_document_asks_for_what_most_pages_do() -> None:
    page = respond_on("https", 8443, _Head(_DOCUMENT_PATH)).body.decode()

    for kind in ("style", "script", "image", "font"):
        assert f'rel="preload" href="/case/document-preload-{kind}" as="{kind}"' in page
    assert '<link rel="stylesheet" href="/case/document-style">' in page
    assert '<img src="/case/document-image-small" width="1" height="1" alt="">' in page
    assert '<img src="/case/document-image" alt="">' in page
    assert '<script src="/case/document-script-deferred" defer></script>' in page


def test_requests_with_credentials_can_read_their_answers_and_only_they_get_cookies() -> None:
    answer = respond_on("https", 8443, _Head("/case/credentials-cookie-set"))

    headers = [(name.lower(), value) for name, value in answer.headers]
    assert ("access-control-allow-origin", _PAGE_ORIGIN) in headers
    assert ("access-control-allow-credentials", "true") in headers
    assert ("access-control-allow-origin", "*") not in headers
    assert [value for name, value in headers if name == "set-cookie"] == [
        f"mitto=1; Path=/case/fetch-credentials-{mode}" for mode in ("omit", "same-origin", "include")
    ]
    for mode in ("omit", "same-origin", "include"):
        assert _answer_to(f"/case/fetch-credentials-{mode}")["Access-Control-Allow-Origin"] == _PAGE_ORIGIN


def test_the_preflight_is_answered_so_that_the_request_after_it_goes() -> None:
    answer = _answer_to("/case/fetch-post-json-cross-site")

    assert answer["Access-Control-Allow-Origin"] == "*"
    assert answer["Access-Control-Allow-Headers"] == "content-type"


def test_the_cookie_of_the_traversal_back_is_set_for_its_page_alone_and_cleared() -> None:
    assert _answer_to("/case/history-cookie-set")["Set-Cookie"] == "mitto=1; Path=/case/navigation-history-back"
    assert _answer_to("/case/history-cookie-clear")["Set-Cookie"] == (
        "mitto=; Max-Age=0; Path=/case/navigation-history-back"
    )


def test_the_frames_page_sets_a_cookie_of_each_same_site_for_the_nested_frame_alone() -> None:
    answer = respond_on("https", 8443, _Head("/case/documents-frames"))

    cookies = [value for name, value in answer.headers if name == "Set-Cookie"]
    assert cookies == [
        f"{same_site.lower()}=1; Path=/case/{path}; Secure; SameSite={same_site}"
        for path in ("frame-nested", "frame-nested-fetch")
        for same_site in ("Strict", "Lax", "None")
    ]
    frame = respond_on("https", 8443, _Head("/case/frame-cross-site")).body.decode()
    assert f'<iframe src="{_PAGE_ORIGIN}/case/frame-nested"></iframe>' in frame
