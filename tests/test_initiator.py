"""Tests of what the page a request comes from does to it, on each hop, through the public API. They need a native library.

The Parity Checks compare what Chrome sent for the cases it recorded, over each protocol. These cover what no Baseline holds.
Each rule comes from Chrome's source, as the native core's fetch_rules.cc and sec_header_helpers.cc say. Every name resolves
to local servers, so a test can use names that say how they relate.
"""

import http.server
import ssl
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, quote, urlsplit

import pytest

import mitto
from mitto import Cookie, CookiePartitionKey, RequestError
from mitto._native import PrivateSettings
from tests.local_server import LocalHTTPServer
from tools.capture import CROSS_SITE_HOST, HTTP1, HTTP2, HTTP3, CaptureServer
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("native_library", "both_apis")

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="

HOST = "www.example.test"
SAME_SITE = "other.example.test"
CROSS_SITE = "www.other.test"
ANOTHER_SITE = "www.third.test"

ERR_INVALID_ARGUMENT = -4
ERR_INVALID_URL = -300

# Longer than any local request takes, even on a busy machine.
TIMEOUT_SECONDS = 30.0

ALL_KINDS = list(mitto.RequestKind)
SUBRESOURCES = [mitto.RequestKind.FETCH, mitto.RequestKind.SCRIPT, mitto.RequestKind.IMAGE]
NO_CORS = [mitto.RequestKind.SCRIPT, mitto.RequestKind.IMAGE]

CLIENT_HINTS = ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"]
FETCH_METADATA = {
    mitto.RequestKind.NAVIGATION: ["Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-User", "Sec-Fetch-Dest"],
    mitto.RequestKind.FETCH: ["Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest"],
    mitto.RequestKind.SCRIPT: ["Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest"],
    mitto.RequestKind.IMAGE: ["Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest"],
}

STRICT = "strict=1; Secure; SameSite=Strict"
LAX = "lax=1; Secure; SameSite=Lax"
NONE = "none=1; Secure; SameSite=None"
ALL_COOKIES = (STRICT, LAX, NONE)


@dataclass(frozen=True, slots=True)
class Received:
    method: str
    path: str
    headers: list[tuple[str, str]]
    client_port: int

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
    """Records a request. Its query can name a status, a place to redirect to and Set-Cookie lines to answer with."""

    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(RecordingServer, self.server)
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append(Received(self.command, self.path, self.headers.items(), self.client_address[1]))
        query = parse_qs(urlsplit(self.path).query)
        destination = query.get("to")
        status = int(query["status"][0]) if "status" in query else (302 if destination else 200)
        self.send_response(status)
        for line in query.get("set", []):
            self.send_header("Set-Cookie", line)
        if destination:
            self.send_header("Location", destination[0])
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        self.answer()

    def do_HEAD(self) -> None:
        self.answer()

    def do_POST(self) -> None:
        self.answer()

    def do_PUT(self) -> None:
        self.answer()

    def do_DELETE(self) -> None:
        self.answer()

    def do_OPTIONS(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@dataclass(frozen=True, slots=True)
class Servers:
    """One HTTPS and one HTTP server, which every name reaches."""

    secure: RecordingServer
    plain: RecordingServer

    def https(self, host: str = HOST, path: str = "/") -> str:
        return f"https://{host}:{self.secure.server_port}{path}"

    def http(self, host: str = HOST, path: str = "/") -> str:
        return f"http://{host}:{self.plain.server_port}{path}"

    @property
    def received(self) -> list[Received]:
        """What the HTTPS server has received."""
        return self.secure.received


@pytest.fixture
def servers() -> Iterator[Servers]:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(PRIVATE_CERTIFICATE)
    with RecordingServer() as secure, RecordingServer() as plain:
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        for server in (secure, plain):
            threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield Servers(secure, plain)
        secure.shutdown()
        plain.shutdown()


def new_session() -> mitto.Session:
    return mitto.Session(
        timeout=TIMEOUT_SECONDS,
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
        ),
    )


def setting(url: str, *lines: str) -> str:
    """Return `url` with a query that makes the server answer with these Set-Cookie lines."""
    return f"{url}?{'&'.join(f'set={quote(line, safe="")}' for line in lines)}"


def redirect(url: str, to: str, status: int = 302) -> str:
    """Return `url` with a query that makes the server redirect to another URL."""
    return f"{url}?status={status}&to={quote(to, safe='')}"


def chain(first: str, *rest: str, status: int = 302) -> str:
    """Return the first of these URLs with a query that makes the server go through the others in turn."""
    url = rest[-1] if rest else first
    for hop in reversed((first, *rest[:-1])) if rest else ():
        url = redirect(hop, url, status)
    return url


# Sec-Fetch-Site


@pytest.mark.parametrize("kind", ALL_KINDS)
@pytest.mark.parametrize(
    ("host", "relation"),
    [(HOST, "same-origin"), (SAME_SITE, "same-site"), (CROSS_SITE, "cross-site")],
)
def test_sec_fetch_site_says_how_the_url_relates_to_the_initiator(
    servers: Servers, kind: mitto.RequestKind, host: str, relation: str
) -> None:
    with new_session() as session:
        session.get(servers.https(host), kind=kind, initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Site") == relation


def test_a_navigation_with_no_initiator_counts_as_started_by_the_browser(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https())

    received = servers.received[0]
    assert received.header("Sec-Fetch-Site") == "none"
    assert received.header("Referer") is None


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_with_no_initiator_counts_as_from_the_root_of_its_origin(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        session.get(servers.https(path="/some/path?query"), kind=kind)

    received = servers.received[0]
    assert received.header("Sec-Fetch-Site") == "same-origin"
    assert received.header("Referer") == servers.https()
    assert received.header("Origin") is None


def test_another_scheme_on_the_same_name_is_cross_site(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(), kind="script", initiator=servers.http(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Site") == "cross-site"


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_sec_fetch_site_is_the_furthest_the_chain_has_been(servers: Servers, kind: mitto.RequestKind) -> None:
    page = servers.https(HOST, "/page")
    with new_session() as session:
        session.get(
            chain(
                servers.https(HOST), servers.https(SAME_SITE), servers.https(CROSS_SITE), servers.https(HOST, "/end")
            ),
            kind=kind,
            initiator=page,
        )

    assert [received.header("Sec-Fetch-Site") for received in servers.received] == [
        "same-origin",
        "same-site",
        "cross-site",
        "cross-site",
    ]


def test_sec_fetch_site_stays_none_through_a_redirect_when_the_browser_started_it(servers: Servers) -> None:
    with new_session() as session:
        session.get(chain(servers.https(HOST), servers.https(CROSS_SITE)))

    assert [received.header("Sec-Fetch-Site") for received in servers.received] == ["none", "none"]


# Referer


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_the_referer_is_the_whole_page_on_the_same_origin(servers: Servers, kind: mitto.RequestKind) -> None:
    page = f"https://user:secret@{HOST}:{servers.secure.server_port}/page?a=b#fragment"
    with new_session() as session:
        session.get(servers.https(), kind=kind, initiator=page)

    assert servers.received[0].header("Referer") == servers.https(path="/page?a=b")


@pytest.mark.parametrize("kind", ALL_KINDS)
@pytest.mark.parametrize("host", [SAME_SITE, CROSS_SITE])
def test_the_referer_is_the_origin_of_the_page_on_another_origin(
    servers: Servers, kind: mitto.RequestKind, host: str
) -> None:
    with new_session() as session:
        session.get(servers.https(host), kind=kind, initiator=servers.https(HOST, "/page?a=b"))

    assert servers.received[0].header("Referer") == servers.https(HOST)


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_no_referer_goes_from_https_to_http(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.http(), kind=kind, initiator=servers.https(HOST, "/page"))

    assert servers.plain.received[0].header("Referer") is None


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_a_referer_goes_from_http_to_https_as_an_origin(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.https(), kind=kind, initiator=servers.http(HOST, "/page"))

    assert servers.received[0].header("Referer") == servers.http(HOST)


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_the_referer_shrinks_at_the_first_other_origin_and_stays_small(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(HOST, "/end")),
            kind=kind,
            initiator=servers.https(HOST, "/page"),
        )

    assert [received.header("Referer") for received in servers.received] == [
        servers.https(HOST, "/page"),
        servers.https(HOST),
        servers.https(HOST),
    ]


def test_the_referer_goes_at_a_redirect_from_https_to_http(servers: Servers) -> None:
    with new_session() as session:
        session.get(redirect(servers.https(), servers.http()), kind="script", initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Referer") == servers.https(HOST, "/page")
    assert servers.plain.received[0].header("Referer") is None


def test_a_referer_from_a_caller_goes_as_it_is_to_every_hop(servers: Servers) -> None:
    referer = "https://somewhere.test/a/path"
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE), servers.http()),
            kind="fetch",
            initiator=servers.https(HOST, "/page"),
            headers={"referer": referer},
        )

    assert [received.header("Referer") for received in [*servers.received, *servers.plain.received]] == [referer] * 3


def test_a_caller_can_remove_the_referer_from_every_hop(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE)),
            kind="fetch",
            initiator=servers.https(HOST, "/page"),
            headers={"Referer": None},
        )

    assert [received.header("Referer") for received in servers.received] == [None, None]


# Origin


@pytest.mark.parametrize(
    ("method", "host", "has_origin"),
    [
        ("GET", HOST, False),
        ("HEAD", HOST, False),
        ("GET", SAME_SITE, True),
        ("HEAD", SAME_SITE, True),
        ("GET", CROSS_SITE, True),
        ("POST", HOST, True),
        ("PUT", HOST, True),
        ("DELETE", HOST, True),
        ("OPTIONS", HOST, True),
        ("POST", CROSS_SITE, True),
    ],
)
def test_a_fetch_has_an_origin_for_another_origin_or_a_method_but_get_and_head(
    servers: Servers, method: str, host: str, has_origin: bool
) -> None:
    # A POST here has no body, and Chrome still sends the Origin.
    with new_session() as session:
        session.request(method, servers.https(host), kind="fetch", initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Origin") == (servers.https(HOST).removesuffix("/") if has_origin else None)


@pytest.mark.parametrize("kind", NO_CORS)
@pytest.mark.parametrize("host", [HOST, CROSS_SITE])
def test_a_script_or_image_has_no_origin(servers: Servers, kind: mitto.RequestKind, host: str) -> None:
    with new_session() as session:
        session.get(servers.https(host), kind=kind, initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Origin") is None


@pytest.mark.parametrize("host", [HOST, SAME_SITE, CROSS_SITE])
def test_a_navigation_by_get_has_no_origin(servers: Servers, host: str) -> None:
    with new_session() as session:
        session.get(servers.https(host), kind="navigation", initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Origin") is None


def test_a_form_says_the_origin_of_its_page(servers: Servers) -> None:
    with new_session() as session:
        session.post(
            servers.https(CROSS_SITE), kind="navigation", data={"a": "b"}, initiator=servers.https(HOST, "/page")
        )

    assert servers.received[0].header("Origin") == servers.https(HOST).removesuffix("/")


def test_a_post_navigation_with_no_initiator_has_an_opaque_origin(servers: Servers) -> None:
    with new_session() as session:
        session.post(servers.https(), kind="navigation", data={"a": "b"})

    assert servers.received[0].header("Origin") == "null"


def test_a_form_from_https_to_http_has_an_opaque_origin(servers: Servers) -> None:
    with new_session() as session:
        session.post(servers.http(), kind="navigation", data={"a": "b"}, initiator=servers.https(HOST, "/page"))

    assert servers.plain.received[0].header("Origin") == "null"


# What an Origin holds in Chrome, as url::Origin::Serialize() writes it. "ß" stays in UTS 46, where IDNA 2003 writes "ss".
@pytest.mark.parametrize(
    ("page", "origin"),
    [
        ("https://example.test:8444/any/path?query#fragment", "https://example.test:8444"),
        ("https://example.test/", "https://example.test"),
        ("https://example.test:443/", "https://example.test"),
        ("http://example.test:80/", "http://example.test"),
        ("http://example.test:443/", "http://example.test:443"),
        ("https://example.test:80/", "https://example.test:80"),
        ("HTTPS://EXAMPLE.Test/", "https://example.test"),
        ("https://user:password@example.test/", "https://example.test"),
        ("https://[::1]:8444/", "https://[::1]:8444"),
        ("https://[::1]/", "https://[::1]"),
        ("https://bücher.example/", "https://xn--bcher-kva.example"),
        ("https://faß.example/", "https://xn--fa-hia.example"),
        ("data:text/plain,hello", "null"),
    ],
)
def test_the_origin_of_the_page_is_written_as_chrome_writes_it(servers: Servers, page: str, origin: str) -> None:
    with new_session() as session:
        session.post(servers.https(), kind="fetch", data="x", initiator=page)

    assert servers.received[0].header("Origin") == origin


def test_a_subresource_with_no_initiator_gets_the_origin_of_its_url_in_uts_46(servers: Servers) -> None:
    with new_session() as session:
        session.post(f"https://faß.example:{servers.secure.server_port}/", kind="fetch", data="x")

    assert servers.received[0].header("Origin") == f"https://xn--fa-hia.example:{servers.secure.server_port}"


def test_a_fetch_says_null_when_a_redirect_comes_back_from_another_origin(servers: Servers) -> None:
    # This is what the Baseline of redirect_fetch_4 has. Leaving another origin than the page's counts, whatever for.
    page = servers.https(HOST, "/page")
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(HOST, "/end")),
            kind="fetch",
            initiator=page,
        )

    origin = servers.https(HOST).removesuffix("/")
    assert [received.header("Origin") for received in servers.received] == [None, origin, "null"]


def test_a_fetch_says_null_once_a_redirect_leaves_another_origin_for_a_third(servers: Servers) -> None:
    page = servers.https(HOST, "/page")
    hops = [servers.https(HOST), servers.https(SAME_SITE), servers.https(CROSS_SITE), servers.https(HOST, "/end")]
    with new_session() as session:
        session.get(chain(*hops), kind="fetch", initiator=page)

    origin = servers.https(HOST).removesuffix("/")
    assert [received.header("Origin") for received in servers.received] == [None, origin, "null", "null"]


def test_a_script_gets_no_origin_on_a_redirect(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE)), kind="script", initiator=servers.https(HOST, "/page")
        )

    assert [received.header("Origin") for received in servers.received] == [None, None]


@pytest.mark.parametrize("status", [307, 308])
def test_a_fetch_that_keeps_its_method_says_the_page_on_another_origin(servers: Servers, status: int) -> None:
    with new_session() as session:
        session.post(
            redirect(servers.https(HOST), servers.https(CROSS_SITE), status),
            kind="fetch",
            data="x",
            initiator=servers.https(HOST, "/page"),
        )

    origin = servers.https(HOST).removesuffix("/")
    assert [received.header("Origin") for received in servers.received] == [origin, origin]


@pytest.mark.parametrize("status", [307, 308])
def test_a_form_that_keeps_its_method_says_null_on_another_origin(servers: Servers, status: int) -> None:
    # //net does this, for what Fetch calls a redirect tainting a request.
    with new_session() as session:
        session.post(
            redirect(servers.https(HOST), servers.https(CROSS_SITE), status),
            kind="navigation",
            data="x",
            initiator=servers.https(HOST, "/page"),
        )

    origin = servers.https(HOST).removesuffix("/")
    assert [received.method for received in servers.received] == ["POST", "POST"]
    assert [received.header("Origin") for received in servers.received] == [origin, "null"]


def test_a_form_keeps_its_origin_on_a_redirect_to_the_same_origin(servers: Servers) -> None:
    with new_session() as session:
        session.post(
            redirect(servers.https(HOST), servers.https(HOST, "/end"), 307),
            kind="navigation",
            data="x",
            initiator=servers.https(HOST, "/page"),
        )

    origin = servers.https(HOST).removesuffix("/")
    assert [received.header("Origin") for received in servers.received] == [origin, origin]


@pytest.mark.parametrize(
    ("kind", "host", "origin_of_the_second_hop"),
    [
        ("navigation", HOST, False),
        ("navigation", CROSS_SITE, False),
        ("fetch", HOST, False),
        # A fetch to another origin always says where it comes from, also as a GET.
        ("fetch", CROSS_SITE, True),
    ],
)
def test_a_redirect_that_makes_a_post_a_get(
    servers: Servers, kind: str, host: str, origin_of_the_second_hop: bool
) -> None:
    with new_session() as session:
        session.post(
            redirect(servers.https(HOST), servers.https(host, "/end"), 303),
            kind=kind,
            data="x",
            initiator=servers.https(HOST, "/page"),
        )

    origin = servers.https(HOST).removesuffix("/")
    assert [received.method for received in servers.received] == ["POST", "GET"]
    assert servers.received[1].header("Origin") == (origin if origin_of_the_second_hop else None)


def test_an_origin_that_a_redirect_adds_goes_after_the_other_lines(servers: Servers) -> None:
    # This is where the Baseline of redirect_fetch_2 has it, and a first hop has it elsewhere.
    with new_session() as session:
        session.get(
            redirect(servers.https(HOST), servers.https(CROSS_SITE)),
            kind="fetch",
            initiator=servers.https(HOST, "/page"),
        )

    names = servers.received[1].names
    assert names.index("Origin") == names.index("Sec-Fetch-Dest") + 1


@pytest.mark.parametrize("kind", ALL_KINDS)
@pytest.mark.parametrize("method", ["GET", "POST"])
def test_no_placeholder_of_a_persona_reaches_the_wire(servers: Servers, kind: mitto.RequestKind, method: str) -> None:
    with new_session() as session:
        session.request(method, servers.https(), kind=kind)

    assert [line for line in servers.received[0].headers if "<" in line[1]] == []


# Sec-Fetch-Storage-Access


@pytest.mark.parametrize("kind", NO_CORS)
def test_a_cross_site_script_or_image_says_its_storage_access(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.https(CROSS_SITE), kind=kind, initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Storage-Access") == "active"
    names = servers.received[0].names
    assert names.index("Sec-Fetch-Storage-Access") == names.index("Sec-Fetch-Dest") + 1


@pytest.mark.parametrize("kind", NO_CORS)
@pytest.mark.parametrize("host", [HOST, SAME_SITE])
def test_a_same_site_script_or_image_says_nothing_of_storage_access(
    servers: Servers, kind: mitto.RequestKind, host: str
) -> None:
    with new_session() as session:
        session.get(servers.https(host), kind=kind, initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Storage-Access") is None


def test_a_cross_site_fetch_says_nothing_of_storage_access_because_it_sends_no_credentials(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(CROSS_SITE), kind="fetch", initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Storage-Access") is None


def test_a_cross_site_navigation_says_nothing_of_storage_access_because_it_becomes_the_first_party(
    servers: Servers,
) -> None:
    with new_session() as session:
        session.get(servers.https(CROSS_SITE), kind="navigation", initiator=servers.https(HOST, "/page"))

    assert servers.received[0].header("Sec-Fetch-Storage-Access") is None


def test_storage_access_follows_each_hop_of_a_script(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(HOST, "/end")),
            kind="script",
            initiator=servers.https(HOST, "/page"),
        )

    assert [received.header("Sec-Fetch-Storage-Access") for received in servers.received] == [None, "active", None]


def test_a_url_that_is_not_potentially_trustworthy_says_nothing_of_storage_access(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.http(CROSS_SITE), kind="script", initiator=servers.https(HOST, "/page"))

    assert servers.plain.received[0].header("Sec-Fetch-Storage-Access") is None


# What is not potentially trustworthy


def _sec_names(received: Received) -> list[str]:
    return [name for name in received.names if name.lower().startswith(("sec-ch-", "sec-fetch-"))]


@pytest.mark.parametrize("kind", ALL_KINDS)
@pytest.mark.parametrize("with_page", [False, True], ids=["no-initiator", "initiator"])
def test_plain_http_to_a_name_on_a_network_gets_no_client_hints_or_fetch_metadata(
    servers: Servers, kind: mitto.RequestKind, with_page: bool
) -> None:
    with new_session() as session:
        session.get(servers.http(), kind=kind, initiator=servers.http(HOST, "/page") if with_page else None)

    assert _sec_names(servers.plain.received[0]) == []


def test_mitto_get_leaves_them_out_too(servers: Servers) -> None:
    # It takes no settings, so it needs an address that is no local host and still reaches the server. This one is both.
    mitto.get(f"http://0.0.0.0:{servers.plain.server_port}/")

    assert _sec_names(servers.plain.received[0]) == []


@pytest.mark.parametrize("host", ["127.0.0.1", "localhost", "name.localhost"])
@pytest.mark.parametrize("kind", ALL_KINDS)
def test_plain_http_to_a_local_host_is_potentially_trustworthy(
    servers: Servers, host: str, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        session.get(servers.http(host), kind=kind)

    assert _sec_names(servers.plain.received[0]) == [
        *[name for name in _persona_order(kind) if name in CLIENT_HINTS or name.startswith("Sec-Fetch-")]
    ]


def _persona_order(kind: mitto.RequestKind) -> list[str]:
    if kind == mitto.RequestKind.NAVIGATION:
        return [*CLIENT_HINTS, *FETCH_METADATA[kind]]
    return ["sec-ch-ua-platform", "sec-ch-ua", "sec-ch-ua-mobile", *FETCH_METADATA[kind]]


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_a_redirect_from_https_to_http_takes_them_away(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(redirect(servers.https(), servers.http()), kind=kind, initiator=servers.https(HOST, "/page"))

    assert _sec_names(servers.plain.received[0]) == []
    assert servers.plain.received[0].header("User-Agent") == servers.received[0].header("User-Agent")


def test_a_navigation_from_http_to_https_gets_them_in_chromes_order(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            redirect(servers.http(), servers.https()), kind="navigation", initiator=servers.https(HOST, "/page")
        )

    assert _sec_names(servers.plain.received[0]) == []
    received = servers.received[0]
    assert _sec_names(received) == [*FETCH_METADATA[mitto.RequestKind.NAVIGATION], *CLIENT_HINTS]
    assert received.header("Sec-Fetch-User") == "?1"
    assert received.header("Sec-Fetch-Dest") == "document"
    assert received.header("Sec-Fetch-Mode") == "navigate"
    # The plain HTTP hop was on another scheme, which makes it another site.
    assert received.header("Sec-Fetch-Site") == "cross-site"
    assert received.header("sec-ch-ua-mobile") == "?0"


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_from_http_to_https_gets_fetch_metadata_and_no_client_hints(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    # Blink adds its client hints when it starts a request, and the network service adds the rest on each hop.
    with new_session() as session:
        session.get(redirect(servers.http(), servers.https()), kind=kind, initiator=servers.http(HOST, "/page"))

    # The page's site has another scheme, so the URL is third-party to it, which a request with credentials says.
    storage_access = ["Sec-Fetch-Storage-Access"] if kind in NO_CORS else []
    assert _sec_names(servers.received[0]) == [*FETCH_METADATA[kind], *storage_access]
    assert servers.received[0].header("Sec-Fetch-Site") == "cross-site"


def test_client_hints_of_a_subresource_stay_gone_after_a_url_that_is_not_potentially_trustworthy(
    servers: Servers,
) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(), servers.http(), servers.https(HOST, "/end")),
            kind="script",
            initiator=servers.https(HOST, "/page"),
        )

    first, last = servers.received
    assert [name for name in _sec_names(first) if name.startswith("sec-ch-")] != []
    assert _sec_names(last) == FETCH_METADATA[mitto.RequestKind.SCRIPT]


def test_client_hints_of_a_navigation_come_back_after_a_url_that_is_not_potentially_trustworthy(
    servers: Servers,
) -> None:
    with new_session() as session:
        session.get(chain(servers.https(), servers.http(), servers.https(HOST, "/end")), kind="navigation")

    assert _sec_names(servers.received[1]) == [*FETCH_METADATA[mitto.RequestKind.NAVIGATION], *CLIENT_HINTS]


# Where the lines go on a hop


@pytest.mark.parametrize("hop", [1, 2], ids=["second-hop", "third-hop"])
def test_a_navigation_sends_client_hints_after_fetch_metadata_from_the_second_hop(servers: Servers, hop: int) -> None:
    with new_session() as session:
        session.get(chain(servers.https(HOST), servers.https(SAME_SITE), servers.https(CROSS_SITE)), kind="navigation")

    first = _sec_names(servers.received[0])
    assert first == [*CLIENT_HINTS, *FETCH_METADATA[mitto.RequestKind.NAVIGATION]]
    assert _sec_names(servers.received[hop]) == [*FETCH_METADATA[mitto.RequestKind.NAVIGATION], *CLIENT_HINTS]


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_keeps_its_client_hints_in_place_on_a_hop(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(chain(servers.https(HOST), servers.https(HOST, "/end")), kind=kind)

    assert _sec_names(servers.received[0]) == _sec_names(servers.received[1]) == _persona_order(kind)


# A caller's lines


@pytest.mark.parametrize(
    "name",
    ["Sec-Fetch-Site", "sec-fetch-site", "Sec-Fetch-Dest", "Sec-Fetch-Mode", "Sec-Fetch-User", "sec-ch-ua", "Origin"],
)
def test_a_callers_line_stays_as_given_on_each_hop(servers: Servers, name: str) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE)),
            kind="navigation",
            initiator=servers.https(HOST, "/page"),
            headers={name: "mine"},
        )

    # //net still changes an Origin on a redirect to another origin, as it does in Chrome.
    assert [received.header(name) for received in servers.received] == (
        ["mine", "null"] if name == "Origin" else ["mine", "mine"]
    )


@pytest.mark.parametrize(
    "name", ["Sec-Fetch-Site", "Sec-Fetch-Dest", "Sec-Fetch-Mode", "Sec-Fetch-User", "sec-ch-ua", "Origin"]
)
def test_a_line_that_a_caller_removes_stays_gone_on_each_hop(servers: Servers, name: str) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE)),
            kind="navigation",
            initiator=servers.https(HOST, "/page"),
            headers={name: None},
        )

    assert [received.header(name) for received in servers.received] == [None, None]


def test_a_callers_origin_on_a_get_goes_out_where_the_rules_send_none(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(), kind="fetch", headers={"Origin": "https://mine.test"})

    assert servers.received[0].header("Origin") == "https://mine.test"


def test_a_callers_origin_is_not_replaced_by_the_one_a_fetch_would_add_on_a_redirect(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE)),
            kind="fetch",
            initiator=servers.https(HOST, "/page"),
            headers={"Origin": None},
        )

    assert [received.header("Origin") for received in servers.received] == [None, None]


def test_a_callers_sec_header_goes_over_plain_http(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.http(), headers={"Sec-Fetch-Site": "mine", "sec-ch-custom": "1"})

    assert _sec_names(servers.plain.received[0]) == ["Sec-Fetch-Site", "sec-ch-custom"]


# Mistakes


@pytest.mark.parametrize("initiator", ["not a url", "", "http://[bad/"])
def test_an_initiator_that_is_not_a_url_is_refused(servers: Servers, initiator: str) -> None:
    with new_session() as session, pytest.raises(RequestError) as caught:
        session.get(servers.https(), initiator=initiator)

    assert caught.value.net_error == ERR_INVALID_ARGUMENT
    assert servers.received == []


def test_an_initiator_with_a_nul_is_refused(servers: Servers) -> None:
    with new_session() as session, pytest.raises(ValueError, match=r"^initiator holds a NUL character$"):
        session.get(servers.https(), initiator="https://a.test/\0")

    assert servers.received == []


@pytest.mark.parametrize("kind", ALL_KINDS)
@pytest.mark.parametrize("url", ["not a url", "https://", "//example.test/", "https://example.test:99999/", ""])
def test_the_native_core_says_what_is_wrong_with_a_url_of_each_kind(kind: mitto.RequestKind, url: str) -> None:
    with new_session() as session, pytest.raises(RequestError) as caught:
        session.post(url, kind=kind)

    assert caught.value.net_error == ERR_INVALID_URL


# SameSite cookies


def _cookie_names(received: Received) -> set[str]:
    cookie = received.header("Cookie")
    return {pair.partition("=")[0] for pair in cookie.split("; ")} if cookie else set()


@pytest.mark.parametrize(
    ("kind", "host", "expected"),
    [
        # A navigation becomes the first party of what it goes to. So a link from elsewhere only leaves out Strict.
        ("navigation", SAME_SITE, {"strict", "lax", "none"}),
        ("navigation", CROSS_SITE, {"lax", "none"}),
        # A subresource stays with its page.
        ("script", SAME_SITE, {"strict", "lax", "none"}),
        ("script", CROSS_SITE, {"none"}),
        ("image", SAME_SITE, {"strict", "lax", "none"}),
        ("image", CROSS_SITE, {"none"}),
        # A fetch() that does not ask for credentials sends them to its own origin only.
        ("fetch", HOST, {"strict", "lax", "none"}),
        ("fetch", SAME_SITE, set[str]()),
        ("fetch", CROSS_SITE, set[str]()),
    ],
)
def test_which_samesite_cookies_a_request_sends(servers: Servers, kind: str, host: str, expected: set[str]) -> None:
    target = servers.https(host)
    with new_session() as session:
        session.get(setting(target, *ALL_COOKIES))
        servers.received.clear()

        session.get(target, kind=kind, initiator=servers.https(HOST, "/page"))

    assert _cookie_names(servers.received[0]) == expected


@pytest.mark.parametrize(
    ("host", "expected"),
    [(HOST, {"strict", "lax", "none"}), (SAME_SITE, {"strict", "lax", "none"}), (CROSS_SITE, {"none"})],
)
def test_a_form_that_posts_across_sites_leaves_out_lax_too(servers: Servers, host: str, expected: set[str]) -> None:
    target = servers.https(host)
    with new_session() as session:
        session.get(setting(target, *ALL_COOKIES))
        servers.received.clear()

        session.post(target, kind="navigation", data={"a": "b"}, initiator=servers.https(HOST, "/page"))

    assert _cookie_names(servers.received[0]) == expected


def test_a_navigation_the_browser_started_sends_every_cookie(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), *ALL_COOKIES))
        servers.received.clear()

        session.get(servers.https())

    assert _cookie_names(servers.received[0]) == {"strict", "lax", "none"}


@pytest.mark.parametrize(
    ("kind", "host", "expected"),
    [
        ("navigation", SAME_SITE, {"strict", "lax", "none"}),
        ("navigation", CROSS_SITE, {"strict", "lax", "none"}),
        ("script", SAME_SITE, {"strict", "lax", "none"}),
        ("script", CROSS_SITE, {"none"}),
        ("image", SAME_SITE, {"strict", "lax", "none"}),
        ("image", CROSS_SITE, {"none"}),
        ("fetch", HOST, {"strict", "lax", "none"}),
        ("fetch", SAME_SITE, set[str]()),
        ("fetch", CROSS_SITE, set[str]()),
    ],
)
def test_which_samesite_cookies_a_response_sets(servers: Servers, kind: str, host: str, expected: set[str]) -> None:
    with new_session() as session:
        session.get(setting(servers.https(host), *ALL_COOKIES), kind=kind, initiator=servers.https(HOST, "/page"))

        assert {cookie.name for cookie in session.cookies} == expected


def test_a_fetch_loses_its_credentials_for_good_at_another_origin(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), NONE))
        servers.received.clear()

        session.get(
            chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(HOST, "/end")),
            kind="fetch",
            initiator=servers.https(HOST, "/page"),
        )

    assert [_cookie_names(received) for received in servers.received] == [{"none"}, set(), set()]


def test_a_cookie_set_on_a_hop_of_a_cross_site_navigation_counts_as_first_party(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            redirect(servers.https(HOST), setting(servers.https(CROSS_SITE), STRICT)),
            kind="navigation",
            initiator=servers.https(HOST, "/page"),
        )

        assert session.cookies.get("strict") is not None


# Partitioning


def test_a_partitioned_cookie_goes_only_with_the_top_level_site_that_set_it(servers: Servers) -> None:
    target = servers.https(CROSS_SITE)
    line = "a=1; Secure; SameSite=None; Partitioned"
    with new_session() as session:
        session.get(setting(target, line), kind="script", initiator=servers.https(HOST, "/page"))
        servers.received.clear()

        session.get(target, kind="script", initiator=servers.https(HOST, "/another"))
        session.get(target, kind="script", initiator=servers.https(SAME_SITE, "/same-site"))
        session.get(target, kind="script", initiator=servers.https(ANOTHER_SITE, "/page"))
        session.get(target, kind="navigation")

        cookie = session.cookies.get("a")

    assert [_cookie_names(received) for received in servers.received] == [{"a"}, {"a"}, set(), set()]
    assert cookie is not None
    assert cookie.partition_key == CookiePartitionKey("https://example.test", True)


def test_a_partitioned_cookie_of_a_navigation_is_in_the_partition_of_its_own_site(servers: Servers) -> None:
    line = "a=1; Secure; SameSite=None; Partitioned"
    with new_session() as session:
        session.get(setting(servers.https(), line))

        cookie = session.cookies.get("a")

    assert cookie is not None
    assert cookie.partition_key == CookiePartitionKey("https://example.test", False)


def test_a_cookie_from_a_caller_with_a_partition_goes_with_that_partition(servers: Servers) -> None:
    key = CookiePartitionKey("https://example.test", True)
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=CROSS_SITE, secure=True, same_site="none", partition_key=key))

        session.get(servers.https(CROSS_SITE), kind="script", initiator=servers.https(HOST, "/page"))
        session.get(servers.https(CROSS_SITE), kind="script", initiator=servers.https(ANOTHER_SITE, "/page"))

    assert [_cookie_names(received) for received in servers.received] == [{"a"}, set()]


def _client_ports(servers: Servers) -> list[int]:
    return [received.client_port for received in servers.received]


def test_a_navigation_shares_a_connection_with_what_its_page_asks_of_its_own_site(servers: Servers) -> None:
    target = servers.https(ANOTHER_SITE)
    with new_session() as session:
        session.get(target, kind="navigation", initiator=servers.https(HOST, "/page"))
        session.get(target, kind="script", initiator=target)

    assert len(set(_client_ports(servers))) == 1


def test_a_navigation_to_a_site_has_another_connection_than_what_another_site_asks_of_it(servers: Servers) -> None:
    target = servers.https(ANOTHER_SITE)
    with new_session() as session:
        session.get(target, kind="script", initiator=servers.https(HOST, "/page"))
        session.get(target, kind="navigation", initiator=servers.https(HOST, "/page"))

    first, second = _client_ports(servers)
    assert first != second


def test_a_fetch_with_no_credentials_has_another_connection_than_one_with_them(servers: Servers) -> None:
    # //net keeps them apart by privacy mode, in Chrome too.
    target = servers.https(ANOTHER_SITE)
    with new_session() as session:
        session.get(target, kind="script", initiator=servers.https(HOST, "/page"))
        session.get(target, kind="fetch", initiator=servers.https(HOST, "/page"))

    first, second = _client_ports(servers)
    assert first != second


def _connections_to_one_url(protocol: str, *initiators: str) -> int:
    """Return how many connections the capture server gets for a script from each of these pages, on one Session."""
    with CaptureServer() as server:
        run = run_of("script_cross_site", protocol)
        assert run is not None
        url = server.url(run.route, host=CROSS_SITE_HOST, path="/case/script-cross-site")
        with mitto.Session(_settings=session_settings(server, run)) as session:
            for initiator in initiators:
                session.get(url, kind="script", initiator=initiator)
        recording = server.recording(run.route, host=CROSS_SITE_HOST)
    assert sum(len(connection.requests) for connection in recording.connections) == len(initiators)
    return len(recording.connections)


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_two_top_level_sites_do_not_share_a_connection_to_a_third(protocol: str) -> None:
    assert _connections_to_one_url(protocol, "https://a.test/page", "https://b.test/page") == 2


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_one_top_level_site_shares_a_connection_to_a_third(protocol: str) -> None:
    assert _connections_to_one_url(protocol, "https://a.test/page", "https://www.a.test/another") == 1
