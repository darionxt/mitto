"""Tests for a Session's cookies: what //net does with them, the calls that reach its store, and the saved file.

Tests that take the native_library fixture go through the real native core to local servers, so they need no network
connection, and are skipped when this machine has no native library. Every name resolves to those servers, so a test can
use names that say how they relate.
"""

import dataclasses
import http.server
import json
import os
import ssl
import stat
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta, timezone, tzinfo
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, quote, urlsplit

import pytest

import mitto
from mitto import Cookie, CookiePartitionKey, _cookies, _native
from mitto._native import PrivateSettings
from tests.local_server import LocalHTTPServer

pytestmark = pytest.mark.usefixtures("both_apis")

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="

HOST = "www.example.test"
SUBDOMAIN = "sub.www.example.test"
SIBLING = "other.example.test"
OTHER_SITE = "www.other.test"

# Longer than any local request takes, even on a busy machine.
TIMEOUT_SECONDS = 30.0
MAX_AGE_SECONDS = 400 * 24 * 60 * 60
# A cookie's times are set a moment apart from what a test reads from its clock.
CLOCK_SLACK = timedelta(minutes=5)
ANY_TIME = datetime(2026, 1, 2, 3, 4, 5, 678901, tzinfo=UTC)
# The Machine of each Session here, unless a test says otherwise, so that one Session can load what another saved.
MACHINE_SEED = 149
MACHINE = mitto.Machine.draw(seed=MACHINE_SEED)


class CookieHandler(http.server.BaseHTTPRequestHandler):
    """Sets the cookies its query names, redirects if it names where to, and answers with the Cookie header it got."""

    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        query = parse_qs(urlsplit(self.path).query, encoding="latin-1")
        destination = query.get("to", [])
        lines = [b"HTTP/1.1 302 Found" if destination else b"HTTP/1.1 200 OK"]
        lines += [b"Set-Cookie: " + line.encode("latin-1") for line in query.get("set", [])]
        lines += [b"Location: " + url.encode("latin-1") for url in destination]
        # What the Session sent, byte for byte: http.server decodes a header as Latin-1.
        body = self.headers.get("Cookie", "").encode("latin-1")
        lines.append(b"Content-Length: " + str(len(body)).encode())
        self.wfile.write(b"\r\n".join(lines) + b"\r\n\r\n" + body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@dataclass(frozen=True, slots=True)
class Servers:
    """One HTTPS and one HTTP server, which every name reaches."""

    https_port: int
    http_port: int

    def https(self, path: str = "/", host: str = HOST) -> str:
        return f"https://{host}:{self.https_port}{path}"

    def http(self, path: str = "/", host: str = HOST) -> str:
        return f"http://{host}:{self.http_port}{path}"


@pytest.fixture
def servers() -> Iterator[Servers]:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(PRIVATE_CERTIFICATE)
    with (
        LocalHTTPServer(("127.0.0.1", 0), CookieHandler) as https_server,
        LocalHTTPServer(("127.0.0.1", 0), CookieHandler) as http_server,
    ):
        https_server.socket = context.wrap_socket(https_server.socket, server_side=True)
        for server in (https_server, http_server):
            threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield Servers(https_server.server_port, http_server.server_port)
        https_server.shutdown()
        http_server.shutdown()


def new_session(cookie_file: Path | str | None = None, *, machine: mitto.Machine | None = MACHINE) -> mitto.Session:
    return mitto.Session(
        machine=machine,
        timeout=TIMEOUT_SECONDS,
        cookie_file=cookie_file,
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
        ),
    )


def setting(url: str, *lines: str, to: str | None = None) -> str:
    """Return `url` with a query that makes the server answer with these Set-Cookie lines, and a redirect."""
    query = [f"set={quote(line, encoding='latin-1', safe='')}" for line in lines]
    if to is not None:
        query.append(f"to={quote(to, safe='')}")
    return f"{url}?{'&'.join(query)}"


def sent(session: mitto.Session, url: str) -> str:
    """Return the Cookie header that a request to `url` carries."""
    return session.get(url).content.decode("latin-1")


def stored(line: str, *, url: Callable[[Servers], str] = Servers.https, servers: Servers) -> list[Cookie]:
    """Return what a new Session's store holds after one response with this Set-Cookie line."""
    with new_session() as session:
        session.get(setting(url(servers), line))
        return list(session.cookies)


def file_of(cookies: object, *, machine: object = None, **changes: object) -> bytes:
    """Return a file of cookies of version 2 that holds `cookies` and MACHINE, or `machine`, with `changes` made."""
    saved = dataclasses.asdict(MACHINE) if machine is None else machine
    return json.dumps({"version": 2, "machine": saved, "cookies": cookies, **changes}).encode()


def machine_with(**changes: object) -> dict[str, object]:
    """Return MACHINE as a file holds it, with `changes` made, where a value of ... leaves its field out."""
    saved = {**dataclasses.asdict(MACHINE), **changes}
    return {name: value for name, value in saved.items() if value is not ...}


# What //net does with a server's cookies.


@pytest.mark.usefixtures("native_library")
def test_a_set_cookie_reaches_later_requests_and_not_the_one_that_got_it(servers: Servers) -> None:
    with new_session() as session:
        assert sent(session, setting(servers.https(), "a=1")) == ""

        assert sent(session, servers.https()) == "a=1"
        assert sent(session, servers.https("/any/other/path")) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_a_set_cookie_on_a_redirect_reaches_the_next_hop(servers: Servers) -> None:
    with new_session() as session:
        assert sent(session, setting(servers.https(), "a=1", to=servers.https("/next"))) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_a_navigation_the_browser_started_counts_as_the_site_of_each_hop(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(host=OTHER_SITE), "already=1; SameSite=Strict"))

        response = session.get(setting(servers.https(), to=setting(servers.https(host=OTHER_SITE), "added=1")))

        assert response.content == b"already=1"
        assert session.cookies.get("added") is not None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("attribute", ["SameSite=Strict", "SameSite=Lax", "SameSite=None; Secure", "Secure"])
def test_each_cookie_of_its_site_goes_on_a_navigation_the_browser_started(servers: Servers, attribute: str) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), f"a=1; {attribute}"))

        assert sent(session, servers.https()) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_each_kind_of_request_sends_cookies(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1"))

        for kind in mitto.RequestKind:
            assert session.get(servers.https(), kind=kind).content == b"a=1"


@pytest.mark.usefixtures("native_library")
def test_cookies_go_longest_path_first_then_earliest_first(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "first=1"))
        session.get(setting(servers.https(), "second=2"))
        session.get(setting(servers.https(), "longest=3; Path=/x"))

        assert sent(session, servers.https("/x")) == "longest=3; first=1; second=2"


@pytest.mark.usefixtures("native_library")
def test_a_path_keeps_a_cookie_off_other_paths(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Path=/x"))

        assert sent(session, servers.https("/x/y")) == "a=1"
        assert sent(session, servers.https("/z")) == ""


@pytest.mark.usefixtures("native_library")
def test_a_host_only_cookie_stays_on_its_host(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1"))

        assert sent(session, servers.https(host=SUBDOMAIN)) == ""
        assert sent(session, servers.https(host=SIBLING)) == ""


@pytest.mark.usefixtures("native_library")
def test_a_domain_cookie_reaches_the_rest_of_its_domain(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Domain=example.test"))

        assert sent(session, servers.https(host=SUBDOMAIN)) == "a=1"
        assert sent(session, servers.https(host=SIBLING)) == "a=1"
        assert sent(session, servers.https(host=OTHER_SITE)) == ""


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("domain", ["other.test", "sub.www.example.test", "test", "com"])
def test_a_server_cannot_set_a_cookie_on_a_domain_above_its_site_or_elsewhere(servers: Servers, domain: str) -> None:
    assert stored(f"a=1; Domain={domain}", servers=servers) == []


@pytest.mark.usefixtures("native_library")
def test_sessions_do_not_share_cookies(servers: Servers) -> None:
    with new_session() as one, new_session() as other:
        one.get(setting(servers.https(), "a=1"))

        assert sent(other, servers.https()) == ""
        assert len(other.cookies) == 0


@pytest.mark.usefixtures("native_library")
def test_a_cookie_of_the_store_replaces_a_cookie_header_from_the_caller(servers: Servers) -> None:
    with new_session() as session:
        assert session.get(servers.https(), headers={"Cookie": "mine=1"}).content == b"mine=1"

        session.get(setting(servers.https(), "a=1"))

        assert session.get(servers.https(), headers={"Cookie": "mine=1"}).content == b"a=1"


@pytest.mark.usefixtures("native_library")
def test_a_secure_cookie_goes_only_over_https(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Secure"))

        assert sent(session, servers.https()) == "a=1"
        assert sent(session, servers.http()) == ""


@pytest.mark.usefixtures("native_library")
def test_a_secure_cookie_from_http_is_not_stored(servers: Servers) -> None:
    assert stored("a=1; Secure", url=Servers.http, servers=servers) == []


@pytest.mark.usefixtures("native_library")
def test_a_cookie_from_http_goes_over_https_too(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.http(), "a=1"))

        assert sent(session, servers.https()) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_http_cannot_replace_a_secure_cookie(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Secure"))

        session.get(setting(servers.http(), "a=2"))

        assert sent(session, servers.https()) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_an_http_only_cookie_is_stored_as_one_and_goes_on_requests(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; HttpOnly"))

        assert sent(session, servers.https()) == "a=1"
        cookie = session.cookies.get("a")
        assert cookie is not None
        assert cookie.http_only


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "line",
    [
        "__Host-a=1; Secure; Path=/",
        "__Host-a=1; Secure; Path=/; HttpOnly",
        "__Secure-a=1; Secure",
        "__Secure-a=1; Secure; Domain=example.test",
    ],
)
def test_a_cookie_that_keeps_to_its_prefix_is_stored(servers: Servers, line: str) -> None:
    assert [cookie.name for cookie in stored(line, servers=servers)] == [line.partition("=")[0]]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "line",
    [
        "__Host-a=1; Path=/",
        "__Host-a=1; Secure",
        "__Host-a=1; Secure; Path=/x",
        "__Host-a=1; Secure; Path=/; Domain=example.test",
        "__Secure-a=1",
        "__host-a=1; Path=/",
    ],
)
def test_a_cookie_that_breaks_its_prefix_is_not_stored(servers: Servers, line: str) -> None:
    assert stored(line, servers=servers) == []


@pytest.mark.usefixtures("native_library")
def test_a_prefix_needs_https(servers: Servers) -> None:
    assert stored("__Host-a=1; Secure; Path=/", url=Servers.http, servers=servers) == []


@pytest.mark.usefixtures("native_library")
def test_a_cookie_ends_at_its_max_age(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Max-Age=1"))

        time.sleep(1.1)

        assert sent(session, servers.https()) == ""
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("attribute", ["Max-Age=0", "Max-Age=-1", "Expires=Thu, 01 Jan 2015 00:00:00 GMT"])
def test_an_expiry_in_the_past_removes_a_cookie(servers: Servers, attribute: str) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1"))

        session.get(setting(servers.https(), f"a=2; {attribute}"))

        assert sent(session, servers.https()) == ""
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
def test_a_cookie_with_time_left_goes_and_ends_when_it_says(servers: Servers) -> None:
    before = datetime.now(UTC)
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Max-Age=100"))

        assert sent(session, servers.https()) == "a=1"
        cookie = session.cookies.get("a")

    assert cookie is not None
    assert cookie.expires is not None
    assert abs(cookie.expires - (before + timedelta(seconds=100))) < CLOCK_SLACK


@pytest.mark.usefixtures("native_library")
def test_a_cookie_lasts_no_longer_than_400_days(servers: Servers) -> None:
    [cookie] = stored(f"a=1; Max-Age={MAX_AGE_SECONDS * 2}", servers=servers)

    assert cookie.expires is not None
    assert cookie.created is not None
    assert cookie.expires - cookie.created == timedelta(seconds=MAX_AGE_SECONDS)


# The calls that read and change the store.


@pytest.mark.usefixtures("native_library")
def test_a_cookie_from_a_response_lists_with_all_its_attributes(servers: Servers) -> None:
    before = datetime.now(UTC)

    [cookie] = stored(
        "a=1; Domain=example.test; Path=/x; Secure; HttpOnly; SameSite=Lax; Priority=High; Max-Age=100", servers=servers
    )

    assert cookie.name == "a"
    assert cookie.value == "1"
    assert cookie.domain == ".example.test"
    assert cookie.path == "/x"
    assert cookie.secure
    assert cookie.http_only
    assert cookie.same_site == "lax"
    assert cookie.priority == "high"
    assert cookie.partition_key is None
    assert cookie.source_scheme == "secure"
    assert cookie.source_port == servers.https_port
    assert cookie.source_type == "http"
    for moment in (cookie.created, cookie.last_accessed, cookie.last_updated):
        assert moment is not None
        assert abs(moment - before) < CLOCK_SLACK
    assert cookie.expires is not None
    assert abs(cookie.expires - (before + timedelta(seconds=100))) < CLOCK_SLACK


@pytest.mark.usefixtures("native_library")
def test_a_cookie_from_plain_http_says_so(servers: Servers) -> None:
    [cookie] = stored("a=1", url=Servers.http, servers=servers)

    assert cookie.source_scheme == "non_secure"
    assert cookie.source_port == servers.http_port
    assert cookie.domain == HOST
    assert cookie.expires is None
    assert cookie.same_site == "unspecified"
    assert cookie.priority == "medium"


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_reaches_the_wire(servers: Servers) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))

        assert sent(session, servers.https()) == "a=1"
        assert sent(session, servers.http()) == "a=1"
        assert sent(session, servers.https(host=SUBDOMAIN)) == ""


@pytest.mark.usefixtures("native_library")
def test_a_new_domain_cookie_reaches_its_subdomains(servers: Servers) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=".example.test"))

        assert sent(session, servers.https(host=SUBDOMAIN)) == "a=1"


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_gets_what_it_does_not_say() -> None:
    before = datetime.now(UTC)
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        [cookie] = session.cookies

    assert cookie.path == "/"
    assert not cookie.secure
    assert not cookie.http_only
    assert cookie.same_site == "unspecified"
    assert cookie.priority == "medium"
    assert cookie.expires is None
    assert cookie.source_scheme == "secure"
    assert cookie.source_port == 443
    assert cookie.source_type == "other"
    for moment in (cookie.created, cookie.last_accessed, cookie.last_updated):
        assert moment is not None
        assert abs(moment - before) < CLOCK_SLACK


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_keeps_what_it_says() -> None:
    expires = datetime.now(UTC) + timedelta(days=1)
    with new_session() as session:
        session.cookies.set(
            Cookie(
                "__Secure-a",
                "1",
                domain=".example.test",
                path="/x",
                secure=True,
                http_only=True,
                same_site="strict",
                priority="low",
                expires=expires,
            )
        )
        [cookie] = session.cookies

    assert cookie.name == "__Secure-a"
    assert cookie.domain == ".example.test"
    assert cookie.path == "/x"
    assert cookie.secure
    assert cookie.http_only
    assert cookie.same_site == "strict"
    assert cookie.priority == "low"
    assert cookie.expires == expires


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_lasts_no_longer_than_400_days() -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST, expires=datetime.now(UTC) + timedelta(days=4000)))
        [cookie] = session.cookies

    assert cookie.expires is not None
    assert cookie.created is not None
    assert cookie.expires - cookie.created == timedelta(seconds=MAX_AGE_SECONDS)


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_replaces_one_of_the_same_name_domain_and_path(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1"))

        session.cookies.set(Cookie("a", "2", domain=HOST))
        session.cookies.set(Cookie("a", "3", domain=HOST, path="/x"))
        session.cookies.set(Cookie("a", "4", domain=".example.test"))

        assert sorted((cookie.value, cookie.domain, cookie.path) for cookie in session.cookies) == [
            ("2", HOST, "/"),
            ("3", HOST, "/x"),
            ("4", ".example.test", "/"),
        ]


@pytest.mark.usefixtures("native_library")
def test_a_caller_can_replace_a_secure_cookie(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Secure"))

        session.cookies.set(Cookie("a", "2", domain=HOST))

        assert sent(session, servers.http()) == "a=2"


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_that_has_expired_removes_what_it_would_replace(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1"))

        session.cookies.set(Cookie("a", "2", domain=HOST, expires=datetime.now(UTC) - timedelta(days=1)))

        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("domain", "stored_as"),
    [
        ("EXAMPLE.test", "example.test"),
        (".EXAMPLE.test", ".example.test"),
        ("\u4f8b\u3048.jp", "xn--r8jz45g.jp"),
        ("a_b.test", "a_b.test"),
        ("a.test.", "a.test."),
        ("127.0.0.1", "127.0.0.1"),
        ("[::1]", "[::1]"),
    ],
)
def test_a_new_cookie_writes_its_domain_as_a_url_does(domain: str, stored_as: str) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=domain))

        assert [cookie.domain for cookie in session.cookies] == [stored_as]


@pytest.mark.usefixtures("native_library")
def test_a_dot_before_a_public_suffix_makes_no_cookie_for_all_of_it(servers: Servers) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=".test"))

        assert sent(session, servers.https()) == ""
        assert [cookie.domain for cookie in session.cookies] == ["test"]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "cookie",
    [
        Cookie("__Host-a", "1", domain=HOST),
        Cookie("__Host-a", "1", domain=HOST, secure=True, path="/x"),
        Cookie("__Host-a", "1", domain=".example.test", secure=True),
        Cookie("__Secure-a", "1", domain=HOST),
        Cookie("a;b", "1", domain=HOST),
        Cookie("a=b", "1", domain=HOST),
        Cookie("a\n", "1", domain=HOST),
        Cookie("a", "1;2", domain=HOST),
        Cookie("a", "1\n", domain=HOST),
        Cookie("a", "1\x00", domain=HOST),
        Cookie("", "", domain=HOST),
        Cookie("a", "日本", domain=HOST),
        Cookie("日本", "1", domain=HOST),
        Cookie("a", "1", domain=HOST, path="/日本"),
        Cookie("a", "1", domain=HOST, path="x"),
        Cookie("a", "1", domain=""),
        Cookie("a", "1", domain="."),
        Cookie("a", "1", domain=".."),
        Cookie("a", "1", domain="not a name"),
        Cookie("a", "1", domain="a;b"),
        Cookie("a", "1", domain="a/b"),
        Cookie("a", "1", domain="a:80"),
        Cookie("a", "1", domain="user@a.test"),
        Cookie("a", "1", domain="a?b"),
        Cookie("a", "1", domain="a#b"),
        Cookie("a", "1", domain="*.a.test"),
        Cookie("a", "1", domain="a..test"),
        Cookie("a", "1", domain=".not a name"),
        Cookie("a", "1", domain=".%41.test"),
        Cookie("a", "1", domain=".a.test:80"),
        Cookie("a", "1", domain=HOST, partition_key=CookiePartitionKey("https://example.test", False)),
        Cookie("a", "1", domain=HOST, secure=True, partition_key=CookiePartitionKey("not a site", False)),
        Cookie("a", "1", domain=HOST, secure=True, partition_key=CookiePartitionKey("", False)),
        Cookie("a", "1", domain=HOST, same_site="none"),
        Cookie("a", "1", domain=HOST, same_site="none", created=ANY_TIME),
        Cookie("a", "1", domain=HOST, last_accessed=ANY_TIME),
        Cookie("a", "1", domain=HOST, last_updated=ANY_TIME),
        Cookie("a", "1", domain=HOST, source_scheme="secure"),
        Cookie("a", "1", domain=HOST, source_port=443),
        Cookie("a", "1", domain=HOST, source_type="http"),
        Cookie("a", "1", domain=HOST, expires=datetime(1969, 12, 31, tzinfo=UTC)),
        Cookie("a", "1", domain=HOST, created=datetime(1969, 12, 31, tzinfo=UTC)),
        Cookie("a", "1", domain=HOST, created=datetime(2300, 1, 1, tzinfo=UTC)),
        # What only a person who writes a file could send: these are the checks of a cookie from a store.
        Cookie("a", "1", domain="EXAMPLE.test", created=ANY_TIME),
        Cookie("a", "1", domain="", created=ANY_TIME),
        Cookie("a", "1", domain=".", created=ANY_TIME),
        Cookie("a", "1", domain="not a name", created=ANY_TIME),
        Cookie("__Host-a", "1", domain=HOST, created=ANY_TIME),
        Cookie("a", "1", domain=HOST, path="x", created=ANY_TIME),
        Cookie("a", "1", domain=HOST, created=ANY_TIME, source_port=65536),
        Cookie("a", "1", domain=HOST, created=ANY_TIME, source_port=-3),
    ],
    ids=repr,
)
def test_a_cookie_that_the_native_core_refuses(cookie: Cookie) -> None:
    with new_session() as session:
        with pytest.raises(ValueError, match="refused"):
            session.cookies.set(cookie)

        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "changes",
    [
        {"same_site": "sometimes"},
        {"priority": "urgent"},
    ],
)
def test_an_attribute_outside_its_values_is_refused(changes: dict[str, Any]) -> None:
    with new_session() as session, pytest.raises(ValueError, match="refused"):
        session.cookies.set(dataclasses.replace(Cookie("a", "1", domain=HOST), **changes))


@pytest.mark.parametrize(
    ("attributes", "message"),
    [
        ({"name": 5}, "name has to be text, not int"),
        ({"value": b"1"}, "value has to be text, not bytes"),
        ({"domain": None}, "domain has to be text, not NoneType"),
        ({"path": 5}, "path has to be text, not int"),
        ({"secure": "yes"}, "secure has to be True or False, not str"),
        ({"http_only": 1}, "http_only has to be True or False, not int"),
        ({"same_site": None}, "same_site has to be 'unspecified', 'none', 'lax' or 'strict', not NoneType"),
        ({"priority": 5}, "priority has to be 'low', 'medium' or 'high', not int"),
        ({"expires": 1_800_000_000}, "expires has to be a datetime with a timezone or None, not int"),
        ({"expires": date(2030, 1, 1)}, "expires has to be a datetime with a timezone or None, not date"),
        ({"partition_key": "https://example.test"}, "partition_key has to be a CookiePartitionKey or None, not str"),
        ({"created": "2030-01-01T00:00:00Z"}, "created has to be a datetime with a timezone or None, not str"),
        ({"last_accessed": 5.0}, "last_accessed has to be a datetime with a timezone or None, not float"),
        ({"last_updated": 5}, "last_updated has to be a datetime with a timezone or None, not int"),
        ({"source_scheme": 5}, "source_scheme has to be 'unset', 'non_secure', 'secure' or None, not int"),
        ({"source_port": "443"}, "source_port has to be a whole number or None, not str"),
        ({"source_port": 443.0}, "source_port has to be a whole number or None, not float"),
        ({"source_port": True}, "source_port has to be a whole number or None, not bool"),
        ({"source_type": 5}, "source_type has to be 'http', 'script', 'other' or None, not int"),
    ],
)
def test_an_attribute_of_the_wrong_type_is_named(attributes: dict[str, Any], message: str) -> None:
    arguments: dict[str, Any] = {"name": "a", "value": "1", "domain": HOST, **attributes}

    with pytest.raises(TypeError) as caught:
        Cookie(**arguments)

    assert str(caught.value) == message


def test_a_value_of_the_wrong_type_stays_out_of_the_message() -> None:
    with pytest.raises(TypeError) as caught:
        Cookie("a", b"a secret", domain=HOST)  # pyright: ignore[reportArgumentType]

    assert "secret" not in str(caught.value)


@pytest.mark.parametrize(
    ("attributes", "message"),
    [
        ({"top_level_site": 5}, "top_level_site has to be text, not int"),
        ({"has_cross_site_ancestor": "no"}, "has_cross_site_ancestor has to be True or False, not str"),
        ({"has_cross_site_ancestor": 0}, "has_cross_site_ancestor has to be True or False, not int"),
    ],
)
def test_a_partition_key_of_the_wrong_type_is_named(attributes: dict[str, Any], message: str) -> None:
    arguments: dict[str, Any] = {
        "top_level_site": "https://example.test",
        "has_cross_site_ancestor": False,
        **attributes,
    }

    with pytest.raises(TypeError) as caught:
        CookiePartitionKey(**arguments)

    assert str(caught.value) == message


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("method", "arguments", "message"),
    [
        ("get", {"name": 5}, "name has to be text, not int"),
        ("get", {"name": "a", "domain": 5}, "domain has to be text or None, not int"),
        ("get", {"name": "a", "path": b"/"}, "path has to be text or None, not bytes"),
        ("delete", {"name": None}, "name has to be text, not NoneType"),
        ("delete", {"name": "a", "domain": [HOST]}, "domain has to be text or None, not list"),
        ("delete", {"name": "a", "path": 5}, "path has to be text or None, not int"),
        ("set", {"cookie": {"name": "a", "value": "2"}}, "cookie has to be a Cookie, not dict"),
        ("save", {"path": 5}, "path has to be text or a path, not int"),
        ("load", {"path": b"cookies.json"}, "path has to be text or a path, not bytes"),
    ],
)
def test_an_argument_of_the_wrong_type_is_named_and_the_store_stays_as_it_was(
    method: str, arguments: dict[str, Any], message: str
) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        with pytest.raises(TypeError) as caught:
            getattr(session.cookies, method)(**arguments)

        assert [(cookie.name, cookie.value) for cookie in session.cookies] == [("a", "1")]
    assert str(caught.value) == message


class _NoOffset(tzinfo):
    """A timezone that gives no offset, which leaves a time without one, as Python counts it."""

    def utcoffset(self, dt: datetime | None) -> timedelta | None:
        return None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("attribute", ["expires", "created", "last_accessed", "last_updated"])
@pytest.mark.parametrize(
    "time", [datetime(2030, 1, 1), datetime(2030, 1, 1, tzinfo=_NoOffset())], ids=["naive", "no-offset"]
)
def test_a_time_without_a_timezone_is_named_and_nothing_is_set(attribute: str, time: datetime) -> None:
    arguments: dict[str, Any] = {"name": "a", "value": "1", "domain": HOST, attribute: time}

    with new_session() as session:
        with pytest.raises(TypeError) as caught:
            session.cookies.set(Cookie(**arguments))

        assert len(session.cookies) == 0
    assert str(caught.value) == f"{attribute} has to be a datetime with a timezone or None, not a datetime without one"


@pytest.mark.usefixtures("native_library")
def test_a_time_in_any_timezone_is_the_same_time() -> None:
    # Well inside the 400 days that //net brings an expiry down to.
    expires = datetime.now(UTC).replace(microsecond=0) + timedelta(days=30)
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST, expires=expires.astimezone(timezone(timedelta(hours=8)))))
        cookie = session.cookies.get("a")

    assert cookie is not None
    assert cookie.expires == expires


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "cookies",
    [
        b"",
        b"not json",
        b"[]",
        b"{}",
        b'{"version": 1}',
        b'{"cookies": []}',
        b'{"version": 2, "cookies": []}',
        b'{"version": "1", "cookies": []}',
        b'{"version": 1, "cookies": {}}',
        b'{"version": 1, "cookies": [], "unknown": 1}',
        b'{"version": 1, "cookies": [1]}',
        b'{"version": 1, "cookies": [{}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "domain": "a.test"}]}',
        b'{"version": 1, "cookies": [{"value": "1", "domain": "a.test"}]}',
        b'{"version": 1, "cookies": [{"name": 1, "value": "1", "domain": "a.test"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": 1, "domain": "a.test"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "unknown": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "path": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "secure": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "http_only": "yes"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "same_site": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "expires_us": "1"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "expires_us": 0.5}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "expires_us": -1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "expires_us": 1e300}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "partition_key": 1}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "secure": true, '
        b'"partition_key": {"top_level_site": "https://a.test"}}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "secure": true, '
        b'"partition_key": {"top_level_site": "https://a.test", "has_cross_site_ancestor": true, "unknown": 1}}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "created_us": 1, "source_port": "1"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "created_us": 1, "source_scheme": "?"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "1", "domain": "a.test", "created_us": 1, "source_type": "?"}]}',
        # JSON that holds a lone half of a surrogate pair, or that is not UTF-8.
        b'{"version": 1, "cookies": [{"name": "a", "value": "\\ud800", "domain": "a.test"}]}',
        b'{"version": 1, "cookies": [{"name": "a", "value": "\xff", "domain": "a.test"}]}',
    ],
)
def test_cookies_in_a_form_that_the_native_core_refuses(cookies: bytes) -> None:
    with _native.Session() as session, pytest.raises(ValueError, match="refused"):
        session.set_cookies(cookies)


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "refused",
    [
        # What the parser sees, and what only the store's own rules refuse.
        {"name": "b", "value": "1;2", "domain": HOST},
        {"name": "b", "value": "1", "domain": HOST, "same_site": "none"},
        {"name": "b", "value": "1", "domain": HOST, "same_site": "none", "created_us": 1790000000000000},
    ],
)
def test_a_refused_cookie_stops_the_others_of_its_call(refused: dict[str, object]) -> None:
    replacing = {"name": "a", "value": "new", "domain": HOST}
    adding = {"name": "c", "value": "1", "domain": HOST}
    cookies = {"version": _native.COOKIES_VERSION, "cookies": [replacing, adding, refused]}
    with new_session() as session:
        session.cookies.set(Cookie("a", "old", domain=HOST))
        with pytest.raises(ValueError, match="refused"):
            session._native.set_cookies(json.dumps(cookies).encode())  # pyright: ignore[reportPrivateUsage]

        assert [(cookie.name, cookie.value) for cookie in session.cookies] == [("a", "old")]


@pytest.mark.usefixtures("native_library")
def test_get_finds_a_cookie_by_name_domain_and_path() -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "short", domain=HOST))
        session.cookies.set(Cookie("a", "long", domain=HOST, path="/x"))
        session.cookies.set(Cookie("a", "other", domain=SIBLING))

        def value(*args: str, **kwargs: str) -> str | None:
            cookie = session.cookies.get(*args, **kwargs)
            return None if cookie is None else cookie.value

        assert value("a") == "long"
        assert value("a", path="/") in {"short", "other"}
        assert value("a", domain=SIBLING) == "other"
        assert value("a", domain=HOST, path="/") == "short"
        assert value("a", domain="example.test") is None
        assert value("b") is None


@pytest.mark.usefixtures("native_library")
def test_delete_takes_what_matches_and_says_how_many(servers: Servers) -> None:
    with new_session() as session:
        for domain, path in ((HOST, "/"), (HOST, "/x"), (SIBLING, "/")):
            session.cookies.set(Cookie("a", "1", domain=domain, path=path))
        session.cookies.set(Cookie("b", "1", domain=HOST))

        assert session.cookies.delete("missing") == 0
        assert session.cookies.delete("a", domain=SIBLING, path="/x") == 0
        assert session.cookies.delete("a", domain=SIBLING) == 1
        assert session.cookies.delete("a", path="/x") == 1
        assert sent(session, servers.https()) == "a=1; b=1"
        assert session.cookies.delete("a") == 1
        assert [cookie.name for cookie in session.cookies] == ["b"]


@pytest.mark.usefixtures("native_library")
def test_delete_compares_a_domain_as_it_is_written() -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=".example.test"))

        assert session.cookies.delete("a", domain="example.test") == 0
        assert session.cookies.delete("a", domain=".example.test") == 1


@pytest.mark.usefixtures("native_library")
def test_clear_takes_every_cookie() -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        session.cookies.set(Cookie("b", "1", domain=SIBLING, secure=True, http_only=True))

        assert session.cookies.clear() == 2
        assert len(session.cookies) == 0
        assert session.cookies.clear() == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "cookie_filter",
    [
        b"",
        b"not json",
        b"[]",
        b"{}",
        b'{"version": 2}',
        b'{"version": 1, "unknown": 1}',
        b'{"version": 1, "name": 1}',
        b'{"version": 1, "domain": 1}',
        b'{"version": 1, "path": 1}',
        b'{"version": 1, "name": "\\u65e5"}',
    ],
)
def test_a_filter_that_the_native_core_refuses(cookie_filter: bytes) -> None:
    with _native.Session() as session, pytest.raises(ValueError, match="refused"):
        session.delete_cookies(cookie_filter)


@pytest.mark.usefixtures("native_library")
def test_a_closed_session_refuses_each_call(tmp_path: Path) -> None:
    session = new_session()
    session.close()

    with pytest.raises(ValueError, match="closed"):
        list(session.cookies)
    with pytest.raises(ValueError, match="closed"):
        session.cookies.set(Cookie("a", "1", domain=HOST))
    with pytest.raises(ValueError, match="closed"):
        session.cookies.delete("a")
    with pytest.raises(ValueError, match="closed"):
        session.cookies.clear()
    with pytest.raises(ValueError, match="closed"):
        session.cookies.save(tmp_path / "cookies.json")
    assert list(tmp_path.iterdir()) == []


@pytest.mark.usefixtures("native_library")
def test_many_threads_can_use_the_cookies_at_once() -> None:
    with new_session() as session:

        def add(number: int) -> None:
            session.cookies.set(Cookie(f"a{number}", "1", domain=HOST))
            assert session.cookies.get(f"a{number}") is not None

        threads = [threading.Thread(target=add, args=(number,)) for number in range(20)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        assert len(session.cookies) == 20


def test_a_cookie_keeps_its_value_out_of_its_repr() -> None:
    assert "secret" not in repr(Cookie("a", "secret", domain=HOST))


def test_a_cookie_crosses_to_the_native_core_with_only_what_it_says() -> None:
    assert _cookies._to_native(Cookie("a", "1", domain=HOST)) == {  # pyright: ignore[reportPrivateUsage]
        "name": "a",
        "value": "1",
        "domain": HOST,
        "path": "/",
        "secure": False,
        "http_only": False,
        "same_site": "unspecified",
        "priority": "medium",
    }


def test_a_cookie_with_every_attribute_comes_back_from_the_native_form() -> None:
    cookie = Cookie(
        "a",
        "1",
        domain=HOST,
        path="/x",
        secure=True,
        http_only=True,
        same_site="none",
        priority="high",
        expires=ANY_TIME + timedelta(days=1),
        partition_key=CookiePartitionKey("https://example.test", True),
        created=ANY_TIME,
        last_accessed=ANY_TIME + timedelta(microseconds=1),
        last_updated=ANY_TIME + timedelta(microseconds=2),
        source_scheme="non_secure",
        source_port=8443,
        source_type="script",
    )

    native = _cookies._to_native(cookie)  # pyright: ignore[reportPrivateUsage]

    assert _cookies._from_native(native) == cookie  # pyright: ignore[reportPrivateUsage]
    # A JSON number is a double in the native core, which holds these exactly.
    assert all(float(native[key]) == native[key] for key in native if key.endswith("_us"))  # type: ignore[arg-type]


# What a cookie's bytes are.


@pytest.mark.usefixtures("native_library")
def test_bytes_that_are_not_utf_8_stay_as_a_server_sent_them(servers: Servers) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "n\xe9=caf\xe9 \xff\x85"))

        assert sent(session, servers.https()) == "n\xe9=caf\xe9 \xff\x85"
        [cookie] = session.cookies

    assert cookie.name == "n\xe9"
    assert cookie.value == "caf\xe9 \xff\x85"


@pytest.mark.usefixtures("native_library")
def test_utf_8_from_a_server_reads_like_a_response_header_does(servers: Servers) -> None:
    utf_8_as_latin_1 = "日本".encode().decode("latin-1")
    with new_session() as session:
        response = session.get(setting(servers.https(), f"a={utf_8_as_latin_1}"))

        [cookie] = session.cookies

    assert f"a={cookie.value}" == response.headers["Set-Cookie"]
    assert cookie.value.encode("latin-1").decode() == "日本"


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_sends_each_character_as_one_byte(servers: Servers) -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "caf\xe9", domain=HOST))

        assert sent(session, servers.https()) == "a=caf\xe9"


@pytest.mark.usefixtures("native_library")
def test_a_new_cookie_encodes_its_path_as_a_url_does() -> None:
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST, path="/a b\xe9"))

        assert [cookie.path for cookie in session.cookies] == ["/a%20b%E9"]


@pytest.mark.usefixtures("native_library")
def test_a_partitioned_cookie_keeps_its_key_and_is_apart_from_another_partition(servers: Servers) -> None:
    key = CookiePartitionKey("https://example.test", True)
    with new_session() as session:
        session.cookies.set(Cookie("a", "plain", domain=HOST, secure=True))
        session.cookies.set(Cookie("a", "partitioned", domain=HOST, secure=True, partition_key=key))

        assert sent(session, servers.https()) == "a=plain"
        assert {cookie.value: cookie.partition_key for cookie in session.cookies} == {"plain": None, "partitioned": key}


@pytest.mark.usefixtures("native_library")
def test_a_partitioned_set_cookie_goes_in_the_partition_of_its_site(servers: Servers) -> None:
    # tests/test_initiator.py has what another top-level site does.
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Secure; Partitioned"))

        assert sent(session, servers.https()) == "a=1"
        cookie = session.cookies.get("a")
        assert cookie is not None
        assert cookie.partition_key == CookiePartitionKey("https://example.test", False)


# The saved file.


@pytest.mark.usefixtures("native_library")
def test_saved_cookies_come_back_in_a_new_session_and_reach_the_wire(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.get(setting(servers.https(), "a=1", "b=2; Secure; HttpOnly; Path=/x; Max-Age=100"))
        session.get(setting(servers.http(), "c=3; Domain=example.test; SameSite=Lax; Priority=Low"))
        saved = list(session.cookies)
        session.cookies.save(file)

    with new_session() as session:
        session.cookies.load(file)

        assert list(session.cookies) == saved
        assert sent(session, servers.https("/x")) == "b=2; a=1; c=3"
        assert sent(session, servers.http("/x")) == "a=1; c=3"
        assert sent(session, servers.https(host=SIBLING)) == "c=3"


@pytest.mark.usefixtures("native_library")
def test_a_saved_list_is_the_same_after_it_has_loaded_and_saved_again(servers: Servers, tmp_path: Path) -> None:
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Secure; Path=/x; Max-Age=100"))
        session.cookies.set(Cookie("b", "2", domain=".example.test"))
        session.cookies.set(
            Cookie("c", "3", domain=HOST, secure=True, partition_key=CookiePartitionKey("https://a.test", False))
        )
        session.cookies.save(tmp_path / "one.json")

    with new_session() as session:
        session.cookies.load(tmp_path / "one.json")
        session.cookies.save(tmp_path / "two.json")

    assert (tmp_path / "one.json").read_bytes() == (tmp_path / "two.json").read_bytes()


@pytest.mark.usefixtures("native_library")
def test_loading_keeps_the_order_of_a_cookie_header(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        for name in "zyxwvu":
            session.get(setting(servers.https(), f"{name}=1"))
        order = sent(session, servers.https())
        session.cookies.save(file)

    with new_session() as session:
        session.cookies.load(file)

        assert sent(session, servers.https()) == order == "z=1; y=1; x=1; w=1; v=1; u=1"


@pytest.mark.usefixtures("native_library")
def test_a_cookie_listed_by_one_session_can_be_set_in_another(servers: Servers) -> None:
    with new_session() as one, new_session() as other:
        one.get(setting(servers.https(), "a=1; Path=/x; Secure; HttpOnly; SameSite=Strict; Max-Age=100"))
        one.cookies.set(
            Cookie("b", "2", domain=HOST, secure=True, partition_key=CookiePartitionKey("https://a.test", True))
        )

        for cookie in one.cookies:
            other.cookies.set(cookie)

        assert list(other.cookies) == list(one.cookies)


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "line",
    [
        # //net stores each of these from a response, and the checks of a new cookie would refuse or change it. A server
        # must not be able to stop a saved list from loading.
        "a=1; Path=/" + "\xe9" * 1000,
        "a=1; Path=/with a space",
        "a=1; Path=/\xe9",
        "a" * 2000 + "=" + "b" * 2000,
        "=no name",
        "a=" + '"quoted"',
        "a=\xff\xfe",
        "a=1; Domain=example.test; Path=/x; Secure; HttpOnly; SameSite=None; Priority=High; Partitioned",
        "__Host-a=1; Secure; Path=/",
        "__Host-Http-a=1; Secure; Path=/; HttpOnly",
        "__Http-a=1; Secure; HttpOnly",
    ],
)
def test_what_a_server_can_store_can_always_load(servers: Servers, tmp_path: Path, line: str) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.get(setting(servers.https(), line))
        saved = list(session.cookies)
        session.cookies.save(file)
    assert len(saved) == 1

    with new_session() as session:
        session.cookies.load(file)

        assert list(session.cookies) == saved


@pytest.mark.usefixtures("native_library")
def test_a_cookie_from_http_loads_after_a_secure_cookie_of_its_name(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.get(setting(servers.http(), "a=1"))
        # This one lists first, and no response over HTTP could set the other after it.
        session.get(setting(servers.https(), "a=2; Secure; Path=/x"))
        saved = list(session.cookies)
        session.cookies.save(file)
    assert [cookie.source_scheme for cookie in saved] == ["secure", "non_secure"]

    with new_session() as session:
        session.cookies.load(file)

        assert list(session.cookies) == saved


@pytest.mark.usefixtures("native_library")
def test_a_session_cookie_comes_back(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        session.cookies.save(file)

    with new_session() as session:
        session.cookies.load(file)
        cookie = session.cookies.get("a")

    assert cookie is not None
    assert cookie.expires is None


@pytest.mark.usefixtures("native_library")
def test_a_saved_cookie_that_has_expired_since_does_not_come_back(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Max-Age=1"))
        session.cookies.save(file)

    time.sleep(1.1)
    with new_session() as session:
        session.cookies.load(file)

        assert sent(session, servers.https()) == ""
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
def test_an_old_cookie_that_a_person_wrote_lasts_no_longer_than_400_days() -> None:
    created = datetime.now(UTC) - timedelta(days=1)
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST, created=created, expires=created + timedelta(days=4000)))
        [cookie] = session.cookies

    assert cookie.expires == created + timedelta(days=400)


@pytest.mark.usefixtures("native_library")
def test_loading_replaces_what_has_the_same_name_and_keeps_the_rest(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.cookies.set(Cookie("a", "saved", domain=HOST))
        session.cookies.save(file)

    with new_session() as session:
        session.cookies.set(Cookie("a", "new", domain=HOST))
        session.cookies.set(Cookie("b", "new", domain=HOST))

        session.cookies.load(file)

        assert {cookie.name: cookie.value for cookie in session.cookies} == {"a": "saved", "b": "new"}


@pytest.mark.usefixtures("native_library")
def test_loading_a_file_that_is_not_there_raises(tmp_path: Path) -> None:
    with new_session() as session, pytest.raises(FileNotFoundError):
        session.cookies.load(tmp_path / "missing.json")


@pytest.mark.usefixtures("native_library")
def test_loading_what_is_not_cookies_names_the_file_and_adds_nothing(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    file.write_bytes(file_of([{"name": "a", "value": "secret", "domain": "a.test"}, {}]))

    with new_session() as session:
        with pytest.raises(ValueError, match=r"cookies\.json") as caught:
            session.cookies.load(file)

        assert "secret" not in str(caught.value)
        assert "secret" not in str(caught.value.__cause__)
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
def test_a_file_with_a_cookie_only_the_store_refuses_changes_nothing(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    good = {"name": "a", "value": "new", "domain": HOST}
    bad = {"name": "b", "value": "1", "domain": HOST, "same_site": "none"}
    file.write_bytes(file_of([good, bad]))

    with new_session() as session:
        session.cookies.set(Cookie("a", "old", domain=HOST))
        with pytest.raises(ValueError, match=r"cookies\.json"):
            session.cookies.load(file)

        assert [cookie.value for cookie in session.cookies] == ["old"]


@pytest.mark.skipif(sys.platform == "win32", reason="Windows has no mode bits: a file takes the access of its folder")
@pytest.mark.usefixtures("native_library")
def test_save_makes_a_file_only_its_owner_can_use(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    old_umask = os.umask(0)
    try:
        with new_session() as session:
            session.cookies.save(file)
    finally:
        os.umask(old_umask)

    assert stat.S_IMODE(file.stat().st_mode) == 0o600
    assert [path.name for path in tmp_path.iterdir()] == ["cookies.json"]


@pytest.mark.skipif(sys.platform == "win32", reason="Windows has no mode bits: a file takes the access of its folder")
@pytest.mark.usefixtures("native_library")
def test_save_tightens_a_file_that_others_could_read(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    file.write_text("old")
    file.chmod(0o644)

    with new_session() as session:
        session.cookies.save(file)

    assert stat.S_IMODE(file.stat().st_mode) == 0o600


@pytest.mark.usefixtures("native_library")
def test_save_writes_through_a_link_to_the_file_it_names(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    file.write_text("old")
    link = tmp_path / "link.json"
    link.symlink_to(file)

    with new_session() as session:
        session.cookies.save(link)

    assert link.is_symlink()
    assert json.loads(file.read_text())["cookies"] == []


@pytest.mark.usefixtures("native_library")
def test_a_save_that_fails_leaves_the_file_as_it_was(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    file = tmp_path / "cookies.json"
    file.write_text("old")

    def fail(descriptor: int) -> None:
        raise OSError("the disk is full")

    monkeypatch.setattr(os, "fsync", fail)
    with new_session() as session, pytest.raises(OSError, match="the disk is full"):
        session.cookies.save(file)

    assert file.read_text() == "old"
    assert [path.name for path in tmp_path.iterdir()] == ["cookies.json"]


@pytest.mark.usefixtures("native_library")
def test_saving_to_a_directory_that_is_not_there_raises(tmp_path: Path) -> None:
    with new_session() as session, pytest.raises(FileNotFoundError):
        session.cookies.save(tmp_path / "missing" / "cookies.json")


@pytest.mark.usefixtures("native_library")
def test_a_saved_file_holds_the_machine_beside_the_cookies(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        session.cookies.save(file)

    saved = json.loads(file.read_text())
    assert saved.keys() == {"version", "machine", "cookies"}
    assert saved["version"] == 2
    assert saved["machine"] == dataclasses.asdict(MACHINE)
    assert [cookie["name"] for cookie in saved["cookies"]] == ["a"]


@pytest.mark.usefixtures("native_library")
def test_loading_a_file_of_another_machine_raises_and_adds_nothing(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        session.cookies.save(file)

    with new_session(machine=mitto.Machine.draw(seed=MACHINE_SEED + 1)) as session:
        with pytest.raises(ValueError, match=r"cookies\.json holds the cookies of another Machine"):
            session.cookies.load(file)

        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
def test_a_file_from_mitto_0_1_loads_in_a_session_of_its_machine(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        # What mitto 0.1 saved: the native core's list, with no Machine.
        file.write_bytes(session._native.list_cookies())  # pyright: ignore[reportPrivateUsage]
        with pytest.raises(ValueError, match="another Machine"):
            session.cookies.load(file)

    with new_session(machine=mitto.Machine.named("mitto-0.1")) as session:
        session.cookies.load(file)

        assert [cookie.name for cookie in session.cookies] == ["a"]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "contents",
    [
        b'{"version": 1, "cookies": [{"name": "a", "value": "secret", "domain": "a.test"}, {}]}',
        # The native core reads a file of version 1 as it is, and it knows of no Machine.
        json.dumps({"version": 1, "machine": dataclasses.asdict(MACHINE), "cookies": []}).encode(),
    ],
)
def test_a_file_of_version_1_that_the_native_core_refuses_is_named(tmp_path: Path, contents: bytes) -> None:
    file = tmp_path / "cookies.json"
    file.write_bytes(contents)

    with new_session(machine=mitto.Machine.named("mitto-0.1")) as session:
        with pytest.raises(ValueError, match=r"cookies\.json") as caught:
            session.cookies.load(file)

        assert "secret" not in str(caught.value.__cause__)
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "contents",
    [
        file_of([], version=3),
        file_of([], version=True),
        file_of([], version=2.0),
        json.dumps({"version": 2, "cookies": []}).encode(),
        json.dumps({"version": 2, "machine": dataclasses.asdict(MACHINE)}).encode(),
        file_of([], other=1),
        file_of([], machine=machine_with(seed=...)),
        file_of([], machine=machine_with(memory=...)),
        file_of([], machine=machine_with(window=1)),
        file_of([], machine=machine_with(memory=7)),
        file_of([], machine=machine_with(memory="8")),
        file_of([], machine=machine_with(seed=True)),
        file_of([], machine=[]),
        file_of({}),
        b"[]",
        b"[" * 100_000 + b"]" * 100_000,
    ],
)
def test_a_file_that_is_not_one_of_cookies_and_a_machine_is_refused_and_named(tmp_path: Path, contents: bytes) -> None:
    file = tmp_path / "cookies.json"
    file.write_bytes(contents)

    with new_session() as session:
        with pytest.raises(ValueError, match=r"cookies\.json does not hold cookies"):
            session.cookies.load(file)

        assert len(session.cookies) == 0
    with pytest.raises(ValueError, match=r"cookies\.json does not hold cookies"):
        new_session(file, machine=None)
    assert file.read_bytes() == contents


# Session(cookie_file=...).


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_starts_empty_saves_at_close_and_loads_at_open(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file) as session:
        assert len(session.cookies) == 0
        session.get(setting(servers.https(), "a=1"))
        assert not file.exists()

    # Windows has no mode bits: a file takes the access of its folder.
    if sys.platform != "win32":
        assert stat.S_IMODE(file.stat().st_mode) == 0o600
    with new_session(file) as session:
        assert sent(session, servers.https()) == "a=1"
        session.cookies.delete("a")

    with new_session(file) as session:
        assert len(session.cookies) == 0


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "machine",
    [
        MACHINE,
        mitto.Machine.draw(seed=0, scale=1.25),
        mitto.Machine.draw(seed=-1),
        mitto.Machine.draw(seed=2**80),
        mitto.Machine.named("mitto-0.1"),
        dataclasses.replace(MACHINE, seed=None),
    ],
)
def test_a_session_with_a_cookie_file_claims_the_machine_it_holds_exactly(
    tmp_path: Path, machine: mitto.Machine
) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file, machine=machine) as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))

    with new_session(file, machine=None) as session:
        claimed = session.machine
        assert [cookie.name for cookie in session.cookies] == ["a"]

    assert claimed == machine
    # The same types too, so that each hint is written as it was.
    assert repr(claimed) == repr(machine)


@pytest.mark.usefixtures("native_library")
def test_a_session_with_a_cookie_file_that_is_not_there_yet_draws_a_machine_and_saves_it(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file, machine=None) as session:
        drawn = session.machine

    assert drawn.seed is not None
    with new_session(file, machine=None) as session:
        assert session.machine == drawn


@pytest.mark.usefixtures("native_library")
def test_a_machine_equal_to_its_cookie_files_is_the_one_a_session_claims(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file):
        pass

    machine = mitto.Machine.draw(seed=MACHINE_SEED)
    with new_session(file, machine=machine) as session:
        assert session.machine is machine


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "machine",
    [
        mitto.Machine.draw(seed=MACHINE_SEED + 1),
        # The same values with no seed, from which a later mitto could not draw what this one does not know.
        dataclasses.replace(MACHINE, seed=None),
        dataclasses.replace(MACHINE, color_scheme="light" if MACHINE.color_scheme == "dark" else "dark"),
        mitto.Machine.named("mitto-0.1"),
    ],
)
def test_a_machine_that_differs_from_its_cookie_files_stops_the_session_and_the_file_stays(
    tmp_path: Path, machine: mitto.Machine
) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file):
        pass
    contents = file.read_bytes()

    with pytest.raises(ValueError, match=r"cookies\.json holds the cookies of another Machine than `machine` gives"):
        new_session(file, machine=machine)

    assert file.read_bytes() == contents


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_from_mitto_0_1_loads_as_its_machine_and_saves_as_the_new_version(
    servers: Servers, tmp_path: Path
) -> None:
    file = tmp_path / "cookies.json"
    with new_session() as session:
        session.get(setting(servers.https(), "a=1; Max-Age=100"))
        # What mitto 0.1 saved: the native core's list, with no Machine.
        file.write_bytes(session._native.list_cookies())  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(ValueError, match="another Machine"):
        new_session(file)

    with new_session(file, machine=None) as session:
        assert session.machine == mitto.Machine.named("mitto-0.1")
        assert sent(session, servers.https()) == "a=1"

    saved = json.loads(file.read_text())
    assert saved["version"] == 2
    assert saved["machine"] == dataclasses.asdict(mitto.Machine.named("mitto-0.1"))


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_can_be_saved_along_the_way(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    with new_session(file) as session:
        session.cookies.set(Cookie("a", "1", domain=HOST))
        session.cookies.save(file)

        assert len(json.loads(file.read_text())["cookies"]) == 1


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_keeps_to_the_directory_it_was_named_in(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    (tmp_path / "other").mkdir()
    monkeypatch.chdir(tmp_path)
    with new_session("cookies.json"):
        monkeypatch.chdir(tmp_path / "other")

    assert (tmp_path / "cookies.json").exists()
    assert not (tmp_path / "other" / "cookies.json").exists()


@pytest.mark.usefixtures("native_library")
def test_closing_twice_saves_once(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    session = new_session(file)
    session.close()
    file.unlink()

    session.close()

    assert not file.exists()


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_that_holds_something_else_stops_the_session_and_stays(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    file.write_text("not cookies")

    with pytest.raises(ValueError, match=r"cookies\.json"):
        new_session(file)

    assert file.read_text() == "not cookies"


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_that_cannot_be_read_raises(tmp_path: Path) -> None:
    # Windows answers an attempt to open a directory as a file with access denied.
    with pytest.raises(PermissionError if sys.platform == "win32" else IsADirectoryError):
        new_session(tmp_path)


def test_a_cookie_file_that_cannot_be_named_leaves_no_native_session(monkeypatch: pytest.MonkeyPatch) -> None:
    class BrokenPath:
        def __fspath__(self) -> str:
            raise OSError("no such path")

    def fail(**settings: object) -> None:
        pytest.fail("a native Session was created, and nothing would close it")

    monkeypatch.setattr(_native, "Session", fail)

    with pytest.raises(OSError, match="no such path"):
        mitto.Session(cookie_file=BrokenPath())


@pytest.mark.usefixtures("native_library")
def test_a_cookie_file_in_a_loop_of_links_raises(tmp_path: Path) -> None:
    loop = tmp_path / "cookies.json"
    loop.symlink_to(loop)

    # Path.resolve() raises RuntimeError on Python 3.12. Later, it lets the path through and reading it raises OSError.
    with pytest.raises((RuntimeError, OSError)):
        new_session(loop)


@pytest.mark.usefixtures("native_library")
def test_a_save_that_fails_at_close_raises_and_still_closes(servers: Servers, tmp_path: Path) -> None:
    file = tmp_path / "gone" / "cookies.json"
    file.parent.mkdir()
    session = new_session(file)
    file.parent.rmdir()

    with pytest.raises(FileNotFoundError):
        session.close()

    with pytest.raises(mitto.RequestError):
        session.get(servers.https())
    # Nothing is left to save, and nothing raises again.
    session.close()


@pytest.mark.usefixtures("native_library")
def test_a_crash_loses_only_what_came_after_the_last_save(tmp_path: Path) -> None:
    file = tmp_path / "cookies.json"
    script = f"""
import os
import mitto
session = mitto.Session(cookie_file={str(file)!r}, machine=mitto.Machine.draw(seed={MACHINE_SEED}))
session.cookies.set(mitto.Cookie("saved", "1", domain={HOST!r}))
session.cookies.save({str(file)!r})
session.cookies.set(mitto.Cookie("lost", "1", domain={HOST!r}))
os._exit(0)
"""

    subprocess.run([sys.executable, "-c", script], check=True, timeout=TIMEOUT_SECONDS)

    with new_session() as session:
        session.cookies.load(file)

        assert [cookie.name for cookie in session.cookies] == ["saved"]


@pytest.mark.usefixtures("native_library")
def test_the_default_session_keeps_cookies_between_calls(servers: Servers) -> None:
    mitto.get(setting(servers.http(host="127.0.0.1"), "a=1"))

    assert mitto.get(servers.http(host="127.0.0.1")).content == b"a=1"
