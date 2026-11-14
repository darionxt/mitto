"""Tests of the CORS preflight that a request in cors mode makes first, through the public API. They need a native library.

The Parity Checks compare what Chrome sent for a preflight and the request after it. These cover when one goes, what its
response has to say, and the Session's cache of what it allowed, as the native core's cors_preflight.cc ports them from
Chrome's network service. Every name resolves to a local server, which answers a preflight as the URL's query says.
"""

import base64
import http.server
import ssl
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, urlencode, urlsplit

import pytest

import mitto
from mitto import RequestError, RequestTimeoutError
from mitto._native import PrivateSettings
from tests.local_certificates import serving
from tests.local_proxy import LocalProxy
from tests.local_server import LocalHTTPServer
from tools.capture import CASE_HOST, CROSS_SITE_HOST, DOCUMENTS_START_PATH, HTTP2, CaptureServer
from tools.capture.cases import has_header

pytestmark = pytest.mark.usefixtures("native_library", "both_apis")

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="

HOST = "www.example.test"
CROSS_SITE = "www.other.test"

ERR_FAILED = -2

# Longer than any local request takes, even on a busy machine.
TIMEOUT_SECONDS = 30.0

# What a preflight answers by default: the page's origin, for whatever the request asks.
ALLOW_EVERYTHING = {"acao": "*", "acam": "*", "acah": "*"}


@dataclass(frozen=True, slots=True)
class Received:
    method: str
    path: str
    headers: list[tuple[str, str]]

    def header(self, name: str) -> str | None:
        return next((value for sent, value in self.headers if sent.lower() == name.lower()), None)

    @property
    def names(self) -> list[str]:
        return [name for name, _ in self.headers]


class RecordingServer(LocalHTTPServer):
    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), RecordingHandler)
        self.received: list[Received] = []


class RecordingHandler(http.server.BaseHTTPRequestHandler):
    """Records a request. Its query says how to answer a preflight: acao, acac, acam, acah, max_age and status, and
    stall, the seconds to wait first. With to, any request is redirected there. Any other request gets 200 with
    Access-Control-Allow-Origin: *."""

    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(RecordingServer, self.server)
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append(Received(self.command, self.path, self.headers.items()))
        query = {name: values[0] for name, values in parse_qs(urlsplit(self.path).query).items()}
        if "to" in query:
            self.send_response(307)
            self.send_header("Location", query["to"])
        elif self.command != "OPTIONS":
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
        else:
            time.sleep(float(query.get("stall", "0")))
            self.send_response(int(query.get("status", "200")))
            for key, name in (
                ("acao", "Access-Control-Allow-Origin"),
                ("acac", "Access-Control-Allow-Credentials"),
                ("acam", "Access-Control-Allow-Methods"),
                ("acah", "Access-Control-Allow-Headers"),
                ("max_age", "Access-Control-Max-Age"),
            ):
                if key in query:
                    self.send_header(name, query[key])
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        self.answer()

    def do_POST(self) -> None:
        self.answer()

    def do_PUT(self) -> None:
        self.answer()

    def do_OPTIONS(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@dataclass(frozen=True, slots=True)
class Server:
    secure: RecordingServer

    def https(self, host: str = HOST, path: str = "/", **answer: str) -> str:
        url = f"https://{host}:{self.secure.server_port}{path}"
        return f"{url}?{urlencode(answer)}" if answer else url

    @property
    def received(self) -> list[Received]:
        return self.secure.received


@pytest.fixture
def server() -> Iterator[Server]:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(PRIVATE_CERTIFICATE)
    with RecordingServer() as secure:
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        threading.Thread(target=secure.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield Server(secure)
        secure.shutdown()


def new_session() -> mitto.Session:
    return mitto.Session(
        timeout=TIMEOUT_SECONDS,
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
        ),
    )


def page(server: Server) -> str:
    return server.https(HOST, "/page")


def methods(server: Server) -> list[str]:
    return [received.method for received in server.received]


# When one goes


@pytest.mark.parametrize("kind", ["fetch", "xhr"])
def test_a_cross_origin_request_with_a_line_that_is_not_safelisted_goes_after_a_preflight(
    server: Server, kind: str
) -> None:
    with new_session() as session:
        session.post(server.https(CROSS_SITE, **ALLOW_EVERYTHING), kind=kind, json={"a": 1}, initiator=page(server))

    assert methods(server) == ["OPTIONS", "POST"]


def test_a_method_that_is_not_safelisted_needs_one(server: Server) -> None:
    with new_session() as session:
        session.put(server.https(CROSS_SITE, **ALLOW_EVERYTHING), kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT"]
    assert server.received[0].header("Access-Control-Request-Method") == "PUT"
    assert server.received[0].header("Access-Control-Request-Headers") is None


@pytest.mark.parametrize(
    ("method", "body"),
    [("GET", {}), ("POST", {"data": "text"}), ("POST", {"data": {"a": "b"}})],
    ids=["get", "text", "form"],
)
def test_a_safelisted_request_needs_none(server: Server, method: str, body: dict[str, Any]) -> None:
    with new_session() as session:
        session.request(method, server.https(CROSS_SITE), kind="fetch", initiator=page(server), **body)

    assert methods(server) == [method]


def test_a_request_to_its_pages_origin_needs_none(server: Server) -> None:
    with new_session() as session:
        session.put(server.https(HOST), kind="fetch", json={"a": 1}, initiator=page(server))

    assert methods(server) == ["PUT"]


@pytest.mark.parametrize("kind", ["script", "image", "beacon", "navigation"])
def test_a_request_that_is_not_in_cors_mode_needs_none(server: Server, kind: str) -> None:
    with new_session() as session:
        session.get(server.https(CROSS_SITE), kind=kind, headers={"X-Custom": "1"}, initiator=page(server))

    assert methods(server) == ["GET"]


def test_a_callers_line_counts_as_a_scripts(server: Server) -> None:
    with new_session() as session:
        session.get(
            server.https(CROSS_SITE, **ALLOW_EVERYTHING),
            kind="fetch",
            headers={"X-Custom": "1", "Content-Language": "en"},
            initiator=page(server),
        )

    assert methods(server) == ["OPTIONS", "GET"]
    assert server.received[0].header("Access-Control-Request-Headers") == "x-custom"


# What it sends


def test_the_preflight_sends_chromes_lines_in_chromes_order(server: Server) -> None:
    with new_session() as session:
        session.post(
            server.https(CROSS_SITE, **ALLOW_EVERYTHING),
            kind="fetch",
            json={"a": 1},
            headers={"X-B": "1", "x-a": "2"},
            initiator=page(server),
        )
        user_agent = dict(server.received[1].headers)["User-Agent"]

    preflight = server.received[0]
    assert preflight.names == [
        "Host",
        "Connection",
        "Accept",
        "Access-Control-Request-Method",
        "Access-Control-Request-Headers",
        "Origin",
        "User-Agent",
        "Sec-Fetch-Mode",
        "Sec-Fetch-Site",
        "Sec-Fetch-Dest",
        "Referer",
        "Accept-Encoding",
        "Accept-Language",
    ]
    assert dict(preflight.headers) | {"Host": "", "Accept-Language": ""} == {
        "Host": "",
        "Connection": "keep-alive",
        "Accept": "*/*",
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "content-type,x-a,x-b",
        "Origin": f"https://{HOST}:{server.secure.server_port}",
        "User-Agent": user_agent,
        "Sec-Fetch-Mode": "cors",
        "Sec-Fetch-Site": "cross-site",
        "Sec-Fetch-Dest": "empty",
        "Referer": f"https://{HOST}:{server.secure.server_port}/",
        "Accept-Encoding": "gzip, deflate, br, zstd",
        "Accept-Language": "",
    }


def test_the_preflight_sends_no_cookie(server: Server) -> None:
    with new_session() as session:
        session.cookies.set(mitto.Cookie("a", "1", domain=CROSS_SITE, secure=True, same_site="none"))
        session.put(server.https(CROSS_SITE, **ALLOW_EVERYTHING), kind="fetch", initiator=page(server))

    assert server.received[0].header("Cookie") is None


# What its response has to say


@pytest.mark.parametrize(
    "answer",
    [
        {"acam": "*", "acah": "*"},
        {"acao": "https://another.test", "acam": "*", "acah": "*"},
        {"acao": "*", "acah": "*"},
        {"acao": "*", "acam": "PUT"},
        {"acao": "*", "acam": "*", "acah": "x-other"},
        {"acao": "*", "acam": "*", "acah": "*", "status": "500"},
        {"acao": "*", "acam": "P U T", "acah": "*"},
    ],
    ids=["no-origin", "another-origin", "no-method", "no-header", "another-header", "not-ok", "not-a-token"],
)
def test_a_preflight_that_does_not_allow_the_request_ends_it_with_nothing_sent(
    server: Server, answer: dict[str, str]
) -> None:
    with new_session() as session, pytest.raises(RequestError) as caught:
        session.put(server.https(CROSS_SITE, **answer), kind="fetch", headers={"X-Custom": "1"}, initiator=page(server))

    assert caught.value.net_error == ERR_FAILED
    assert methods(server) == ["OPTIONS"]


def test_a_preflight_that_names_the_origin_the_method_and_the_header_allows_the_request(server: Server) -> None:
    origin = f"https://{HOST}:{server.secure.server_port}"
    with new_session() as session:
        session.put(
            server.https(CROSS_SITE, acao=origin, acam="PUT", acah="X-Custom"),
            kind="fetch",
            headers={"X-Custom": "1"},
            initiator=page(server),
        )

    assert methods(server) == ["OPTIONS", "PUT"]


def test_a_redirect_of_the_preflight_ends_the_request(server: Server) -> None:
    with new_session() as session, pytest.raises(RequestError) as caught:
        session.put(
            server.https(CROSS_SITE, to=server.https(CROSS_SITE, "/other")), kind="fetch", initiator=page(server)
        )

    assert caught.value.net_error == ERR_FAILED
    assert methods(server) == ["OPTIONS"]


# The cache


def test_the_session_keeps_what_a_preflight_allowed(server: Server) -> None:
    url = server.https(CROSS_SITE, **ALLOW_EVERYTHING)
    with new_session() as session:
        session.put(url, kind="fetch", initiator=page(server))
        session.put(url, kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT", "PUT"]


def test_a_preflight_of_no_max_age_lasts_no_time(server: Server) -> None:
    url = server.https(CROSS_SITE, **ALLOW_EVERYTHING, max_age="0")
    with new_session() as session:
        session.put(url, kind="fetch", initiator=page(server))
        session.put(url, kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT", "OPTIONS", "PUT"]


def test_what_was_allowed_counts_for_its_page_and_url_only(server: Server) -> None:
    url = server.https(CROSS_SITE, **ALLOW_EVERYTHING)
    with new_session() as session:
        session.put(url, kind="fetch", initiator=page(server))
        session.put(url, kind="fetch", initiator=server.https("www.third.test", "/page"))
        session.put(server.https(CROSS_SITE, "/elsewhere", **ALLOW_EVERYTHING), kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT", "OPTIONS", "PUT", "OPTIONS", "PUT"]


def test_each_session_has_its_own(server: Server) -> None:
    url = server.https(CROSS_SITE, **ALLOW_EVERYTHING)
    with new_session() as session:
        session.put(url, kind="fetch", initiator=page(server))
    with new_session() as session:
        session.put(url, kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT", "OPTIONS", "PUT"]


def test_a_cached_result_that_does_not_cover_a_new_line_makes_a_new_preflight(server: Server) -> None:
    url = server.https(CROSS_SITE, acao="*", acam="*", acah="x-one")
    with new_session() as session:
        session.put(url, kind="fetch", headers={"X-One": "1"}, initiator=page(server))
        with pytest.raises(RequestError):
            session.put(url, kind="fetch", headers={"X-Two": "1"}, initiator=page(server))

    assert methods(server) == ["OPTIONS", "PUT", "OPTIONS"]


# A redirect


def _requests(server: Server) -> list[tuple[str, str, str]]:
    return [
        (received.method, received.header("Host") or "", received.path.partition("?")[0])
        for received in server.received
    ]


def test_a_redirect_to_another_origin_waits_for_a_preflight_there(server: Server) -> None:
    other = f"{CROSS_SITE}:{server.secure.server_port}"
    with new_session() as session:
        session.post(
            server.https(HOST, "/hop", to=server.https(CROSS_SITE, "/end", **ALLOW_EVERYTHING)),
            kind="fetch",
            json={"a": 1},
            initiator=page(server),
        )

    assert _requests(server) == [
        ("POST", f"{HOST}:{server.secure.server_port}", "/hop"),
        ("OPTIONS", other, "/end"),
        ("POST", other, "/end"),
    ]
    assert server.received[1].header("Referer") == server.https(HOST)


def test_a_redirect_whose_preflight_does_not_allow_the_request_goes_no_further(server: Server) -> None:
    with new_session() as session, pytest.raises(RequestError) as caught:
        session.post(
            server.https(HOST, "/hop", to=server.https(CROSS_SITE, "/end")),
            kind="fetch",
            json={"a": 1},
            initiator=page(server),
        )

    assert caught.value.net_error == ERR_FAILED
    assert [method for method, _, _ in _requests(server)] == ["POST", "OPTIONS"]


# Timers and a proxy


def test_a_preflight_that_does_not_answer_in_time_ends_the_request(server: Server) -> None:
    session = mitto.Session(
        timeout=mitto.Timeout(read=0.5),
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
        ),
    )
    with session, pytest.raises(RequestTimeoutError):
        session.put(server.https(CROSS_SITE, **ALLOW_EVERYTHING, stall="3"), kind="fetch", initiator=page(server))

    assert methods(server) == ["OPTIONS"]


def test_a_preflight_at_a_redirect_that_does_not_answer_in_time_ends_the_request(server: Server) -> None:
    session = mitto.Session(
        timeout=mitto.Timeout(read=0.5),
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
        ),
    )
    with session, pytest.raises(RequestTimeoutError):
        session.post(
            server.https(HOST, "/hop", to=server.https(CROSS_SITE, "/end", **ALLOW_EVERYTHING, stall="3")),
            kind="fetch",
            json={"a": 1},
            initiator=page(server),
        )

    assert methods(server) == ["POST", "OPTIONS"]


_PROXY_USER = "someone"
_PROXY_PASSWORD = "hunter2"
_PROXY_BASIC = "Basic " + base64.b64encode(f"{_PROXY_USER}:{_PROXY_PASSWORD}".encode()).decode()


def test_a_preflight_answers_a_proxy_that_asks_for_credentials() -> None:
    with (
        serving(LocalProxy(authorization=_PROXY_BASIC)) as proxy,
        CaptureServer() as capture,
        mitto.Session(
            proxy=f"http://{_PROXY_USER}:{_PROXY_PASSWORD}@127.0.0.1:{proxy.port}",
            _settings=PrivateSettings(ignore_certificate_errors_spki_list=[capture.spki_hash]),
        ) as session,
    ):
        response = session.post(
            capture.url(HTTP2, host=CROSS_SITE_HOST, path="/case/fetch-post-json-cross-site"),
            kind="fetch",
            json={"json": "body"},
            initiator=capture.url(HTTP2, host=CASE_HOST, path=DOCUMENTS_START_PATH),
        )
        recording = capture.recording(HTTP2, host=CROSS_SITE_HOST)

    assert response.status == 200
    requests = [request for connection in recording.connections for request in connection.requests]
    assert [has_header(request, "access-control-request-method") for request in requests] == [True, False]
    assert any(request.header("Proxy-Authorization") == _PROXY_BASIC for request in proxy.requests)
