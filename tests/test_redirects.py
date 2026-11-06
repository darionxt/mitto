"""Tests of what a redirect does to a request, through the public API. They need a native library."""

import http.server
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from typing import Any, cast
from urllib.parse import parse_qs, urlsplit

import pytest

import mitto
from mitto._native import PrivateSettings
from tests.local_server import LocalHTTPServer
from tools.capture import CASES, HTTP1, HTTP2, HTTP3, CaptureServer, normal_form
from tools.capture.http2 import RST_STREAM
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("native_library")

ERR_UNKNOWN_URL_SCHEME = -302
ERR_INVALID_REDIRECT = -303
ERR_TOO_MANY_REDIRECTS = -310

# What //net allows, in kMaxRedirects.
REDIRECT_LIMIT = 20

REDIRECT_STATUSES = [301, 302, 303, 307, 308]
FINAL_BODY = b"final"
REDIRECT_BODY = b"unwanted"
REQUEST_BODY = b"the body of a request"
SECRET = "Bearer secret"


@dataclass(frozen=True, slots=True)
class ReceivedRequest:
    method: str
    path: str
    headers: dict[str, str]
    body: bytes


class RedirectServer(LocalHTTPServer):
    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), RedirectHandler)
        self.received: list[ReceivedRequest] = []

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}"


class RedirectHandler(http.server.BaseHTTPRequestHandler):
    """Answers /status/CODE?to=URL with a redirect, /chain/COUNT with that many redirects, and anything else with a body."""

    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(RedirectServer, self.server)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append(
            ReceivedRequest(self.command, self.path, {k.lower(): v for k, v in self.headers.items()}, body)
        )
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        parts = url.path.split("/")
        if parts[1] == "status":
            self.send(int(parts[2]), query.get("body", [""])[0].encode(), location=query.get("to", [None])[0])
        elif parts[1] == "chain" and int(parts[2]) > 0:
            self.send(302, b"", location=f"/chain/{int(parts[2]) - 1}")
        elif parts[1] == "loop":
            self.send(302, b"", location="/loop")
        else:
            self.send(200, FINAL_BODY)

    def send(self, status: int, body: bytes, location: str | None = None) -> None:
        self.send_response(status)
        if location is not None:
            self.send_header("Location", location)
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

    def do_DELETE(self) -> None:
        self.answer()

    def do_HEAD(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


def _serve() -> Iterator[RedirectServer]:
    with RedirectServer() as server:
        threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield server
        server.shutdown()


@pytest.fixture
def server() -> Iterator[RedirectServer]:
    yield from _serve()


@pytest.fixture
def other_origin() -> Iterator[RedirectServer]:
    """A second server. Another port is another origin."""
    yield from _serve()


def _methods(server: RedirectServer) -> list[str]:
    return [request.method for request in server.received]


@pytest.mark.parametrize("status", REDIRECT_STATUSES)
def test_a_redirect_is_followed_to_the_response(server: RedirectServer, status: int) -> None:
    start = f"{server.url}/status/{status}?to=/final"

    with mitto.Session() as session:
        response = session.get(start)

    assert response.status == 200
    assert response.url == f"{server.url}/final"
    assert response.content == FINAL_BODY
    [hop] = response.history
    assert (hop.status, hop.url) == (status, start)
    assert hop.headers["location"] == "/final"
    assert _methods(server) == ["GET", "GET"]


@pytest.mark.parametrize("status", REDIRECT_STATUSES)
def test_the_body_of_a_redirect_is_not_the_response(server: RedirectServer, status: int) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}/status/{status}?to=/final&body={REDIRECT_BODY.decode()}")

    assert response.content == FINAL_BODY
    assert response.history[0].headers["content-length"] == str(len(REDIRECT_BODY))


@pytest.mark.parametrize(
    ("method", "status", "next_method"),
    [
        ("POST", 301, "GET"),
        ("POST", 302, "GET"),
        ("POST", 303, "GET"),
        ("POST", 307, "POST"),
        ("POST", 308, "POST"),
        ("PUT", 301, "PUT"),
        ("PUT", 302, "PUT"),
        ("PUT", 303, "GET"),
        ("PUT", 307, "PUT"),
        ("DELETE", 303, "GET"),
        ("HEAD", 301, "HEAD"),
        ("HEAD", 303, "HEAD"),
    ],
)
def test_net_decides_the_method_of_the_next_hop(
    server: RedirectServer, method: str, status: int, next_method: str
) -> None:
    with mitto.Session() as session:
        response = session.request(method, f"{server.url}/status/{status}?to=/final")

    assert response.status == 200
    assert _methods(server) == [method, next_method]


@pytest.mark.parametrize(
    ("status", "next_method", "next_body"),
    [
        (301, "GET", b""),
        (302, "GET", b""),
        (303, "GET", b""),
        (307, "POST", REQUEST_BODY),
        (308, "POST", REQUEST_BODY),
    ],
)
def test_a_body_goes_on_only_with_its_method(
    server: RedirectServer, status: int, next_method: str, next_body: bytes
) -> None:
    with mitto.Session() as session:
        response = session.post(f"{server.url}/status/{status}?to=/final", data=REQUEST_BODY.decode())

    assert response.content == FINAL_BODY
    first, second = server.received
    assert (first.method, first.body) == ("POST", REQUEST_BODY)
    assert (second.method, second.body) == (next_method, next_body)


@pytest.mark.parametrize("status", [301, 302, 303])
def test_a_method_that_changes_leaves_out_what_describes_the_body(server: RedirectServer, status: int) -> None:
    with mitto.Session() as session:
        session.post(f"{server.url}/status/{status}?to=/final", data=REQUEST_BODY.decode())

    first, second = server.received
    assert first.headers["content-length"] == str(len(REQUEST_BODY))
    assert "content-type" in first.headers
    assert {"content-length", "content-type", "origin"}.isdisjoint(second.headers)


@pytest.mark.parametrize("status", [307, 308])
def test_a_method_that_stays_keeps_what_describes_the_body(server: RedirectServer, status: int) -> None:
    with mitto.Session() as session:
        session.post(f"{server.url}/status/{status}?to=/final", data=REQUEST_BODY.decode())

    first, second = server.received
    assert second.headers["content-length"] == str(len(REQUEST_BODY))
    assert second.headers["content-type"] == first.headers["content-type"]


@pytest.mark.parametrize("status", [307, 308])
def test_a_body_is_sent_again_at_each_hop(server: RedirectServer, status: int) -> None:
    second = f"{server.url}/status/{status}?to=/final"

    with mitto.Session() as session:
        session.post(f"{server.url}/status/{status}?to={second}", data=REQUEST_BODY.decode())

    assert [request.body for request in server.received] == [REQUEST_BODY] * 3


def test_a_body_with_no_bytes_goes_on_as_well(server: RedirectServer) -> None:
    with mitto.Session() as session:
        session.post(f"{server.url}/status/307?to=/final", data=b"")

    assert [request.headers["content-length"] for request in server.received] == ["0", "0"]


def test_a_body_goes_to_another_origin_without_authorization(
    server: RedirectServer, other_origin: RedirectServer
) -> None:
    with mitto.Session() as session:
        session.post(
            f"{server.url}/status/307?to={other_origin.url}/final",
            data=REQUEST_BODY.decode(),
            headers={"Authorization": SECRET},
        )

    [received] = other_origin.received
    assert received.body == REQUEST_BODY
    assert "authorization" not in received.headers


@pytest.mark.parametrize("status", REDIRECT_STATUSES)
def test_a_body_is_sent_once_when_a_redirect_is_not_followed(server: RedirectServer, status: int) -> None:
    with mitto.Session() as session:
        response = session.post(
            f"{server.url}/status/{status}?to=/final", data=REQUEST_BODY.decode(), follow_redirects=False
        )

    assert response.status == status
    assert [(request.method, request.body) for request in server.received] == [("POST", REQUEST_BODY)]


def test_the_history_holds_each_hop_in_order(server: RedirectServer, other_origin: RedirectServer) -> None:
    last = f"{other_origin.url}/final"
    second = f"{server.url}/status/307?to={last}"
    first = f"{server.url}/status/301?to={second}"

    with mitto.Session() as session:
        response = session.get(first)

    assert [(hop.status, hop.url) for hop in response.history] == [(301, first), (307, second)]
    assert response.url == last


def test_a_response_with_no_redirect_has_no_history(server: RedirectServer) -> None:
    with mitto.Session() as session:
        assert session.get(f"{server.url}/final").history == ()


def test_the_limit_of_redirects_is_that_of_net(server: RedirectServer) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}/chain/{REDIRECT_LIMIT}")

        assert len(response.history) == REDIRECT_LIMIT
        assert response.url == f"{server.url}/chain/0"

        with pytest.raises(mitto.TooManyRedirectsError):
            session.get(f"{server.url}/chain/{REDIRECT_LIMIT + 1}")


def test_a_redirect_loop_ends_with_an_error(server: RedirectServer) -> None:
    url = f"{server.url}/loop"

    with mitto.Session() as session, pytest.raises(mitto.TooManyRedirectsError) as caught:
        session.get(url)

    assert caught.value.url == url
    assert caught.value.net_error == ERR_TOO_MANY_REDIRECTS
    assert len(server.received) == REDIRECT_LIMIT + 1


@pytest.mark.parametrize("status", REDIRECT_STATUSES)
def test_a_redirect_that_is_not_followed_is_the_response(server: RedirectServer, status: int) -> None:
    url = f"{server.url}/status/{status}?to=/final&body={REDIRECT_BODY.decode()}"

    with mitto.Session() as session:
        response = session.get(url, follow_redirects=False)

    assert response.status == status
    assert response.url == url
    assert response.protocol == "http/1.1"
    assert response.headers["location"] == "/final"
    # //net does not read the body of a redirect, and neither does Chrome.
    assert response.content == b""
    assert response.history == ()
    assert _methods(server) == ["GET"]
    response.raise_for_status()


def test_a_session_goes_on_after_a_redirect_that_is_not_followed(server: RedirectServer) -> None:
    with mitto.Session() as session:
        session.get(f"{server.url}/status/302?to=/final", follow_redirects=False)

        assert session.get(f"{server.url}/final").content == FINAL_BODY


def test_follow_redirects_changes_nothing_for_another_response(server: RedirectServer) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}/final", follow_redirects=False)

    assert response.content == FINAL_BODY


@pytest.mark.parametrize("follow_redirects", [True, False])
def test_a_status_that_does_not_redirect_stays_the_response(server: RedirectServer, follow_redirects: bool) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}/status/300?to=/final", follow_redirects=follow_redirects)

    assert response.status == 300
    assert response.history == ()
    assert _methods(server) == ["GET"]


@pytest.mark.parametrize("follow_redirects", [True, False])
def test_a_redirect_with_no_location_is_the_response(server: RedirectServer, follow_redirects: bool) -> None:
    with mitto.Session() as session:
        response = session.get(f"{server.url}/status/302", follow_redirects=follow_redirects)

    assert response.status == 302
    assert response.history == ()


@pytest.mark.parametrize("follow_redirects", [True, False])
def test_a_redirect_to_a_url_that_is_not_valid_ends_with_an_error(
    server: RedirectServer, follow_redirects: bool
) -> None:
    # //net checks this before it tells its delegate about the redirect.
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.get(f"{server.url}/status/302?to=http://[", follow_redirects=follow_redirects)

    assert caught.value.net_error == ERR_INVALID_REDIRECT


def test_a_redirect_to_a_scheme_that_net_does_not_speak(server: RedirectServer) -> None:
    url = f"{server.url}/status/302?to=data:text/plain,x"

    with mitto.Session() as session:
        assert session.get(url, follow_redirects=False).status == 302

        with pytest.raises(mitto.RequestError) as caught:
            session.get(url)

    assert caught.value.net_error == ERR_UNKNOWN_URL_SCHEME


@pytest.mark.parametrize("name", ["Authorization", "Cookie"])
def test_a_callers_credentials_stay_on_the_same_origin(server: RedirectServer, name: str) -> None:
    with mitto.Session() as session:
        session.get(f"{server.url}/status/302?to=/final", headers={name: SECRET})

    assert [request.headers.get(name.lower()) for request in server.received] == [SECRET, SECRET]


@pytest.mark.parametrize("name", ["Authorization", "authorization", "Cookie", "cookie"])
def test_a_callers_credentials_do_not_go_to_another_origin(
    server: RedirectServer, other_origin: RedirectServer, name: str
) -> None:
    with mitto.Session() as session:
        session.get(f"{server.url}/status/302?to={other_origin.url}/final", headers={name: SECRET, "X-Other": "stays"})

    assert server.received[0].headers[name.lower()] == SECRET
    [received] = other_origin.received
    assert name.lower() not in received.headers
    assert received.headers["x-other"] == "stays"


@pytest.mark.parametrize("name", ["Authorization", "Cookie"])
def test_a_callers_credentials_stay_gone_when_a_hop_comes_back(
    server: RedirectServer, other_origin: RedirectServer, name: str
) -> None:
    back = f"{server.url}/final"

    with mitto.Session() as session:
        session.get(f"{server.url}/status/302?to={other_origin.url}/status/302%3Fto={back}", headers={name: SECRET})

    first, last = server.received
    assert first.headers[name.lower()] == SECRET
    assert last.path == "/final"
    assert name.lower() not in last.headers


def test_a_cookie_from_the_store_goes_to_each_origin_it_fits(
    server: RedirectServer, other_origin: RedirectServer
) -> None:
    # A cookie ignores the port, so it fits both servers, which are different origins.
    with mitto.Session() as session:
        session.cookies.set(mitto.Cookie("stored", "1", domain="127.0.0.1"))

        session.get(f"{server.url}/status/302?to={other_origin.url}/final")

    assert server.received[0].headers["cookie"] == "stored=1"
    assert other_origin.received[0].headers["cookie"] == "stored=1"


def _chain_over(server: CaptureServer, protocol: str) -> tuple[str, PrivateSettings, str, str | None]:
    """Return the first hop of the capture server's redirects over a protocol, how to reach it, and its recording's key."""
    run = run_of("redirect_fetch_1", protocol)
    assert run is not None
    path = next(case.path for case in CASES if case.name == "redirect_fetch_1")
    return server.url(run.route, host=run.host, path=path), session_settings(server, run), run.route, run.host


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
@pytest.mark.parametrize("hops", [3, 0], ids=["followed", "not followed"])
def test_a_redirect_over_each_protocol(protocol: str, hops: int) -> None:
    with CaptureServer() as server:
        url, settings, _, _ = _chain_over(server, protocol)
        with mitto.Session(_settings=settings) as session:
            response = session.get(url, kind=mitto.RequestKind.FETCH, follow_redirects=hops > 0)

    assert response.protocol == protocol
    assert len(response.history) == hops
    assert response.status == (200 if hops else 302)


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_body_is_dropped_at_a_redirect_over_each_protocol(protocol: str) -> None:
    with CaptureServer() as server:
        url, settings, route, host = _chain_over(server, protocol)
        with mitto.Session(_settings=settings) as session:
            response = session.post(url, kind=mitto.RequestKind.FETCH, data=REQUEST_BODY.decode())
        hops_of_a_fetch = [case for case in CASES if case.name.startswith("redirect_fetch_")]
        form = normal_form(server.recording(route, host=host), hops_of_a_fetch)

    assert response.protocol == protocol
    assert len(response.history) == 3
    requests = cast(dict[str, dict[str, Any]], form["requests"])
    assert requests["redirect_fetch_1"]["body"] == REQUEST_BODY.decode()
    assert all("body" not in requests[f"redirect_fetch_{hop}"] for hop in (2, 3, 4))


@pytest.mark.parametrize("hops", [3, 0], ids=["followed", "not followed"])
def test_a_redirect_over_http2_cancels_no_stream(hops: int) -> None:
    # Chrome sends no RST_STREAM here, and a server can see one.
    with CaptureServer() as server:
        url, settings, route, host = _chain_over(server, HTTP2)
        with mitto.Session(_settings=settings) as session:
            session.get(url, kind=mitto.RequestKind.FETCH, follow_redirects=hops > 0)
        connections = server.recording(route, host=host).connections

    assert sum(len(connection.requests) for connection in connections) == hops + 1
    assert [frame for connection in connections for frame in connection.frames if frame.type == RST_STREAM] == []
