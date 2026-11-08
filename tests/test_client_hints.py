"""Tests of the client hints a server asks for, through the public API. They need a native library.

The Parity Checks compare what Chrome sent in the cases it recorded, over each protocol. These cover the rules around them,
which come from Chrome's source, as the native core's client_hints.cc and fetch_rules.cc say. Every name resolves to local
servers, so a test can use names that say how they relate.
"""

import dataclasses
import http.server
import itertools
import json
import ssl
import subprocess
import sys
import threading
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, quote, urlencode, urlsplit

import pytest

import mitto
from mitto import RequestError
from mitto._machine import MACHINE_HINTS, client_hints, draw_noise_salts
from mitto._native import PrivateSettings
from mitto._persona import CHROME_157_WINDOWS, WINDOWS_PERSONAS
from tests.local_server import LocalHTTPServer
from tests.network_noise import browser_downlink, browser_rtt, renderer_downlink, renderer_rtt, renderer_salt
from tools.release.window import recorded_build

pytestmark = pytest.mark.usefixtures("native_library", "both_apis")

PRIVATE_CERTIFICATE = Path(__file__).parent / "data" / "private_certificate.pem"
PRIVATE_CERTIFICATE_SPKI_HASH = "nVtNMMWlVAx2QAb+AXEK53cS8Fm8sDeKSomcP5jydFk="
SAVED_SESSION_CLIENT = Path(__file__).with_name("saved_session_client.py")

HOST = "www.example.test"
SAME_SITE = "other.example.test"
CROSS_SITE = "www.other.test"
ANOTHER_SITE = "www.third.test"

ERR_TOO_MANY_ACCEPT_CH_RESTARTS = -382

# Longer than any local request takes, even on a busy machine.
TIMEOUT_SECONDS = 30.0

ALL_KINDS = list(mitto.RequestKind)
SUBRESOURCES = [mitto.RequestKind.FETCH, mitto.RequestKind.SCRIPT, mitto.RequestKind.IMAGE]

SENT_BY_DEFAULT = ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"]
ASKED_FOR = "sec-ch-ua-arch"
ASKED_FOR_TOO = "sec-ch-ua-model"

# What a navigation sends with every hint, in its order, among what goes anyway.
NAVIGATION_HINTS = [
    "device-memory",
    "sec-ch-device-memory",
    "dpr",
    "sec-ch-dpr",
    "viewport-width",
    "sec-ch-viewport-width",
    "sec-ch-viewport-height",
    "rtt",
    "downlink",
    "ect",
    "sec-ch-ua",
    "sec-ch-ua-mobile",
    "sec-ch-ua-full-version",
    "sec-ch-ua-arch",
    "sec-ch-ua-platform",
    "sec-ch-ua-platform-version",
    "sec-ch-ua-model",
    "sec-ch-ua-bitness",
    "sec-ch-ua-wow64",
    "sec-ch-ua-full-version-list",
    "sec-ch-ua-form-factors",
    "sec-ch-prefers-color-scheme",
    "sec-ch-prefers-reduced-motion",
    "sec-ch-prefers-reduced-transparency",
]
EVERY_HINT = ", ".join([*NAVIGATION_HINTS, "width", "sec-ch-width", "save-data"])


@dataclass(frozen=True, slots=True)
class Received:
    method: str
    path: str
    headers: list[tuple[str, str]]
    body: bytes

    def header(self, name: str) -> str | None:
        return next((value for sent, value in self.headers if sent.lower() == name.lower()), None)

    @property
    def names(self) -> list[str]:
        return [name.lower() for name, _ in self.headers]

    @property
    def hints(self) -> list[str]:
        """The names of the client hints it holds, in their order."""
        known = {*NAVIGATION_HINTS, "width", "sec-ch-width", "save-data"}
        return [name for name in self.names if name in known]


class RecordingServer(LocalHTTPServer):
    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), RecordingHandler)
        self.received: list[Received] = []


class RecordingHandler(http.server.BaseHTTPRequestHandler):
    """Records a request. Its query names a status, a place to redirect to and other headers to answer with."""

    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(RecordingServer, self.server)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append(Received(self.command, self.path, self.headers.items(), body))
        query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
        destination = query.get("to")
        if self.headers.get(query.get("when_sent", [""])[0]) is not None:
            # A request that holds this hint gets another answer: one that asks for another hint, or a redirect.
            query["Accept-CH"] = query["Critical-CH"] = query.get("then", [])
            destination = query.get("then_to", destination)
        status = int(query["status"][0]) if "status" in query else (302 if destination else 200)
        self.send_response(status)
        for name in ("Accept-CH", "Critical-CH", "Clear-Site-Data"):
            for value in query.get(name, []):
                self.send_header(name, value)
        if destination:
            self.send_header("Location", destination[0])
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        self.answer()

    def do_POST(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@dataclass(frozen=True, slots=True)
class Servers:
    """One HTTPS and one HTTP server, which every name reaches."""

    secure: RecordingServer
    plain: RecordingServer

    def https(self, host: str = HOST, path: str = "/", **answer: str) -> str:
        return _with_answer(f"https://{host}:{self.secure.server_port}{path}", answer)

    def http(self, host: str = HOST, path: str = "/", **answer: str) -> str:
        return _with_answer(f"http://{host}:{self.plain.server_port}{path}", answer)

    @property
    def received(self) -> list[Received]:
        """What the HTTPS server has received."""
        return self.secure.received


def _with_answer(url: str, answer: dict[str, str]) -> str:
    """Return `url` with a query that makes the server answer so, where accept_ch stands for Accept-CH and so on."""
    names = {"accept_ch": "Accept-CH", "critical_ch": "Critical-CH", "clear": "Clear-Site-Data"}
    query = {names.get(name, name): value for name, value in answer.items()}
    return f"{url}?{urlencode(query)}" if query else url


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


def new_session(
    persona: str = CHROME_157_WINDOWS,
    *,
    alps: dict[str, str] | None = None,
    machine: mitto.Machine | None = None,
    cookie_file: Path | None = None,
) -> mitto.Session:
    return mitto.Session(
        persona=persona,
        machine=machine,
        timeout=TIMEOUT_SECONDS,
        cookie_file=cookie_file,
        _settings=PrivateSettings(
            ignore_certificate_errors_spki_list=[PRIVATE_CERTIFICATE_SPKI_HASH],
            host_resolver_rules="MAP * 127.0.0.1",
            accept_ch_via_alps=alps or {},
        ),
    )


def redirect(url: str, to: str, **answer: str) -> str:
    """Return `url` with a query that makes the server redirect to another URL, and answer with `answer` as well."""
    separator = "&" if "?" in url else "?"
    extra = _with_answer("", answer).removeprefix("?")
    return f"{url}{separator}to={quote(to, safe='')}{'&' + extra if extra else ''}"


def chain(first: str, *rest: str) -> str:
    """Return the first of these URLs with a query that makes the server go through the others in turn."""
    url = rest[-1]
    for hop in reversed((first, *rest[:-1])):
        url = redirect(hop, url)
    return url


def asks(servers: Servers, session: mitto.Session, host: str = HOST, hints: str = ASKED_FOR) -> str:
    """Have a page at `host` ask for `hints`, and return that page."""
    page = servers.https(host, "/page", accept_ch=hints)
    session.get(page)
    return page


# Asking


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_a_server_that_has_not_asked_gets_only_what_goes_by_default(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.https(), kind=kind)

    assert sorted(servers.received[0].hints) == SENT_BY_DEFAULT


def test_a_navigation_gets_what_its_origin_asked_for_only_after_it_asked(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(path="/page", accept_ch=ASKED_FOR))
        session.get(servers.https())

    assert ASKED_FOR not in servers.received[0].hints
    assert servers.received[1].header(ASKED_FOR) == '"x86"'


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_gets_what_its_origin_asked_for_only_after_it_asked(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        page = servers.https(path="/page")
        session.get(servers.https(path="/first"), kind=kind, initiator=page)
        session.get(page + "?" + urlencode({"Accept-CH": ASKED_FOR}))
        session.get(servers.https(path="/second"), kind=kind, initiator=page)

    assert ASKED_FOR not in servers.received[0].hints
    assert ASKED_FOR in servers.received[2].hints


@pytest.mark.parametrize("host", [SAME_SITE, CROSS_SITE])
def test_what_an_origin_asked_for_does_not_go_to_a_navigation_to_another_origin(servers: Servers, host: str) -> None:
    with new_session() as session:
        page = asks(servers, session)
        session.get(servers.https(host), initiator=page)

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


def test_another_port_is_another_origin(servers: Servers) -> None:
    with RecordingServer() as other, new_session() as session:
        threading.Thread(target=other.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        session.get(servers.http("localhost", "/page", accept_ch=ASKED_FOR))
        session.get(f"http://localhost:{other.server_port}/")
        other.shutdown()

    assert other.received[0].hints == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
@pytest.mark.parametrize("host", [SAME_SITE, CROSS_SITE])
def test_a_page_keeps_what_its_origin_asked_for_from_a_subresource_to_another_origin(
    servers: Servers, kind: mitto.RequestKind, host: str
) -> None:
    with new_session() as session:
        page = asks(servers, session)
        session.get(servers.https(host), kind=kind, initiator=page)

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_goes_by_its_page_and_not_by_its_own_origin(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        asks(servers, session, HOST)
        session.get(servers.https(HOST), kind=kind, initiator=servers.https(CROSS_SITE, "/page"))

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_with_no_initiator_counts_as_from_the_root_of_its_own_origin(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(servers.https(path="/any"), kind=kind)

    assert ASKED_FOR in servers.received[1].hints


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_what_a_subresource_is_answered_counts_for_nothing(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.https(path="/asks", accept_ch=ASKED_FOR), kind=kind)
        session.get(servers.https())

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


def test_each_session_has_its_own(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
    with new_session() as session:
        session.get(servers.https())

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


# What Accept-CH holds


def test_the_last_accept_ch_replaces_what_an_origin_asked_for(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, hints=ASKED_FOR)
        asks(servers, session, hints=ASKED_FOR_TOO)
        session.get(servers.https())

    assert ASKED_FOR not in servers.received[2].hints
    assert ASKED_FOR_TOO in servers.received[2].hints


def test_an_empty_accept_ch_takes_back_what_an_origin_asked_for(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        asks(servers, session, hints="")
        session.get(servers.https())

    assert sorted(servers.received[2].hints) == SENT_BY_DEFAULT


def test_a_response_with_no_accept_ch_leaves_what_an_origin_asked_for(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(servers.https())
        session.get(servers.https())

    assert ASKED_FOR in servers.received[2].hints


@pytest.mark.parametrize("status", ["204", "205"])
def test_a_navigation_that_shows_nothing_asks_for_nothing(servers: Servers, status: str) -> None:
    with new_session() as session:
        session.get(servers.https(path="/page", accept_ch=ASKED_FOR, status=status))
        session.get(servers.https())

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("status", ["404", "500"])
def test_an_error_page_still_asks(servers: Servers, status: str) -> None:
    with new_session() as session:
        session.get(servers.https(path="/page", accept_ch=ASKED_FOR, status=status))
        session.get(servers.https())

    assert ASKED_FOR in servers.received[1].hints


@pytest.mark.parametrize("value", ['"sec-ch-ua-model"', "sec-ch-ua-model, 1", "(sec-ch-ua-model)", "sec-ch-ua-model,,"])
def test_an_accept_ch_that_is_not_a_list_of_tokens_changes_nothing(servers: Servers, value: str) -> None:
    with new_session() as session:
        asks(servers, session)
        asks(servers, session, hints=value)
        session.get(servers.https())

    assert ASKED_FOR in servers.received[2].hints
    assert ASKED_FOR_TOO not in servers.received[2].hints


def test_accept_ch_takes_names_in_any_case_with_parameters_and_leaves_out_others(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, hints="Sec-CH-UA-Arch;a=1, no-such-hint, SEC-CH-UA-MODEL")
        session.get(servers.https())

    assert [hint for hint in servers.received[1].hints if hint not in SENT_BY_DEFAULT] == [ASKED_FOR, ASKED_FOR_TOO]


def test_a_repeated_accept_ch_counts_as_one_list(servers: Servers) -> None:
    url = servers.https(path="/page") + "?" + urlencode([("Accept-CH", ASKED_FOR), ("Accept-CH", ASKED_FOR_TOO)])
    with new_session() as session:
        session.get(url)
        session.get(servers.https())

    assert {ASKED_FOR, ASKED_FOR_TOO} <= set(servers.received[1].hints)


@pytest.mark.parametrize("hint", ["width", "sec-ch-width", "save-data"])
def test_a_hint_that_depends_on_a_page_or_a_setting_does_not_go(servers: Servers, hint: str) -> None:
    with new_session() as session:
        page = asks(servers, session, hints=hint)
        session.get(servers.https(), kind="image", initiator=page)

    assert hint not in servers.received[1].names


# Redirects


def test_a_navigation_gets_on_each_hop_what_the_origin_of_that_hop_asked_for(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, CROSS_SITE)
        session.get(chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(SAME_SITE)))

    first, second, third = servers.received[1:]
    assert sorted(first.hints) == SENT_BY_DEFAULT
    assert second.hints == ["sec-ch-ua", "sec-ch-ua-mobile", ASKED_FOR, "sec-ch-ua-platform"]
    assert sorted(third.hints) == SENT_BY_DEFAULT


def test_a_navigation_moves_its_hints_after_the_fetch_metadata_on_a_hop(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(redirect(servers.https(path="/first"), servers.https(path="/second")))

    first, second = servers.received[1:]
    assert first.names.index(ASKED_FOR) < first.names.index("sec-fetch-site")
    assert second.names.index(ASKED_FOR) > second.names.index("sec-fetch-dest")


def test_what_a_redirect_says_counts_at_once(servers: Servers) -> None:
    with new_session() as session:
        session.get(redirect(servers.https(path="/first"), servers.https(path="/second"), accept_ch=ASKED_FOR))
        session.get(servers.https(path="/third"))

    first, second, third = servers.received
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in second.hints
    assert ASKED_FOR in third.hints


def test_what_a_redirect_says_counts_for_its_own_origin_and_not_for_the_next(servers: Servers) -> None:
    with new_session() as session:
        session.get(redirect(servers.https(HOST), servers.https(CROSS_SITE), accept_ch=ASKED_FOR))
        session.get(servers.https(HOST))

    _, second, third = servers.received
    assert ASKED_FOR not in second.hints
    assert ASKED_FOR in third.hints


def test_what_a_redirect_says_counts_when_it_is_the_response(servers: Servers) -> None:
    with new_session() as session:
        session.get(
            redirect(servers.https(path="/first"), servers.https(path="/second"), accept_ch=ASKED_FOR),
            follow_redirects=False,
        )
        session.get(servers.https(path="/third"))

    first, third = servers.received
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in third.hints


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_loses_what_an_origin_asked_for_at_another_origin_and_does_not_get_it_back(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        page = asks(servers, session)
        session.get(
            chain(servers.https(HOST, "/first"), servers.https(CROSS_SITE), servers.https(HOST, "/last")),
            kind=kind,
            initiator=page,
        )

    first, second, third = servers.received[1:]
    assert ASKED_FOR in first.hints
    assert sorted(second.hints) == SENT_BY_DEFAULT
    assert sorted(third.hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_keeps_what_its_origin_asked_for_on_a_hop_within_it(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        page = asks(servers, session)
        session.get(redirect(servers.https(path="/first"), servers.https(path="/second")), kind=kind, initiator=page)

    first, second = servers.received[1:]
    assert first.hints == second.hints
    assert ASKED_FOR in second.hints


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_what_a_redirect_tells_a_subresource_counts_for_nothing(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(
            redirect(servers.https(path="/first"), servers.https(path="/second"), accept_ch=ASKED_FOR), kind=kind
        )
        session.get(servers.https())

    assert sorted(servers.received[2].hints) == SENT_BY_DEFAULT


# Potentially trustworthy URLs


def test_plain_http_cannot_ask(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.http(path="/page", accept_ch=ASKED_FOR))
        session.get(servers.http())

    assert servers.plain.received[1].hints == []


def test_a_page_on_plain_http_that_asks_does_not_reach_a_redirect_to_https(servers: Servers) -> None:
    with new_session() as session:
        session.get(redirect(servers.http(path="/page"), servers.https(path="/next"), accept_ch=ASKED_FOR))

    assert sorted(servers.received[0].hints) == SENT_BY_DEFAULT


def test_plain_http_to_localhost_is_potentially_trustworthy(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.http("localhost", "/page", accept_ch=ASKED_FOR))
        session.get(servers.http("localhost"))

    assert ASKED_FOR in servers.plain.received[1].hints


def test_a_hint_that_asked_on_https_does_not_go_over_plain_http_on_the_same_name(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(redirect(servers.https(path="/first"), servers.http()))

    assert servers.plain.received[0].hints == []


# Clear-Site-Data


@pytest.mark.parametrize("cleared", ['"clientHints"', '"cookies"', '"cache"', '"*"'])
def test_clear_site_data_takes_back_what_an_origin_asked_for(servers: Servers, cleared: str) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(servers.https(path="/clears", clear=cleared))
        session.get(servers.https())

    assert sorted(servers.received[2].hints) == SENT_BY_DEFAULT


def test_clear_site_data_of_anything_else_leaves_it(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(servers.https(path="/clears", clear='"storage"'))
        session.get(servers.https())

    assert ASKED_FOR in servers.received[2].hints


def test_accept_ch_beside_clear_site_data_counts_for_nothing(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(path="/page", accept_ch=ASKED_FOR, clear='"clientHints"'))
        session.get(servers.https())

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_first_party_subresource_can_clear_what_an_origin_asked_for(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        page = asks(servers, session)
        session.get(servers.https(path="/clears", clear='"clientHints"'), kind=kind, initiator=page)
        session.get(servers.https())

    assert sorted(servers.received[2].hints) == SENT_BY_DEFAULT


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_third_party_subresource_cannot_clear_what_an_origin_asked_for(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(
            servers.https(path="/clears", clear='"clientHints"'),
            kind=kind,
            initiator=servers.https(CROSS_SITE, "/page"),
        )
        session.get(servers.https())

    assert ASKED_FOR in servers.received[2].hints


# What a caller changes


def test_a_callers_hint_goes_as_given_where_nothing_asked_for_it(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(), headers={"Sec-CH-UA-Arch": '"arm"'})

    assert servers.received[0].header(ASKED_FOR) == '"arm"'


def test_a_callers_hint_takes_the_place_of_the_personas_on_each_hop(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, CROSS_SITE)
        session.get(redirect(servers.https(HOST), servers.https(CROSS_SITE)), headers={"Sec-CH-UA-Arch": '"arm"'})

    assert [received.header(ASKED_FOR) for received in servers.received[1:]] == ['"arm"', '"arm"']


def test_a_hint_a_caller_removed_stays_gone(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session)
        session.get(redirect(servers.https(path="/first"), servers.https(path="/second")), headers={ASKED_FOR: None})

    assert all(ASKED_FOR not in received.hints for received in servers.received[1:])


def test_only_the_hints_the_caller_gave_are_the_callers(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, hints=f"{ASKED_FOR}, {ASKED_FOR_TOO}")
        session.get(servers.https(), headers={ASKED_FOR_TOO: '"a model"'})

    received = servers.received[1]
    assert received.header(ASKED_FOR) == '"x86"'
    assert received.header(ASKED_FOR_TOO) == '"a model"'


# The order


def test_a_navigation_sends_every_hint_in_chromes_order(servers: Servers) -> None:
    with new_session() as session:
        asks(servers, session, hints=EVERY_HINT)
        session.get(servers.https())

    assert servers.received[1].hints == NAVIGATION_HINTS


# The order a subresource's lines came in from Chrome 156 on macOS, whatever the values. tests/baselines holds the same for
# other sets from Windows Chrome: the order follows from the names and how many there are.
@pytest.mark.parametrize(
    ("asked", "order"),
    [
        (
            "sec-ch-ua-full-version-list",
            ["sec-ch-ua-full-version-list", "sec-ch-ua-platform", "user-agent", "sec-ch-ua", "sec-ch-ua-mobile"],
        ),
        (
            "sec-ch-prefers-color-scheme, downlink, rtt, sec-ch-ua-form-factors, sec-ch-ua-platform,"
            " sec-ch-ua-platform-version, sec-ch-ua-full-version, sec-ch-ua-arch, sec-ch-ua-model, sec-ch-ua-bitness,"
            " sec-ch-ua-full-version-list, sec-ch-ua-wow64",
            [
                "downlink",
                "sec-ch-ua-full-version-list",
                "sec-ch-ua-platform",
                "sec-ch-ua",
                "sec-ch-ua-bitness",
                "sec-ch-ua-model",
                "sec-ch-ua-mobile",
                "sec-ch-ua-form-factors",
                "sec-ch-ua-wow64",
                "sec-ch-ua-arch",
                "sec-ch-ua-full-version",
                "sec-ch-prefers-color-scheme",
                "user-agent",
                "rtt",
                "sec-ch-ua-platform-version",
            ],
        ),
    ],
    ids=["one", "twelve"],
)
@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_sends_its_lines_in_the_order_of_blinks_map(
    servers: Servers, kind: mitto.RequestKind, asked: str, order: list[str]
) -> None:
    with new_session() as session:
        page = asks(servers, session, hints=asked)
        session.get(servers.https(), kind=kind, initiator=page)

    known = {*order, "accept"}
    assert [name for name in servers.received[1].names if name in known] == [*order, "accept"]


# Each Persona


@pytest.mark.parametrize(
    ("persona", "version"), [(persona, recorded_build(release)) for release, persona in WINDOWS_PERSONAS.items()]
)
def test_each_persona_says_its_own_full_version(servers: Servers, persona: str, version: str) -> None:
    with new_session(persona) as session:
        asks(servers, session, hints="sec-ch-ua-full-version, sec-ch-ua-full-version-list")
        session.get(servers.https())

    received = servers.received[1]
    assert received.header("sec-ch-ua-full-version") == f'"{version}"'
    assert f'v="{version}"' in str(received.header("sec-ch-ua-full-version-list"))


# Critical-CH


def test_a_critical_hint_that_did_not_go_starts_the_navigation_over(servers: Servers) -> None:
    url = servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        response = session.get(url)

    first, second = servers.received
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in second.hints
    assert response.status == 200
    assert second.names.index("accept") < second.names.index(ASKED_FOR) < second.names.index("sec-fetch-site")
    assert second.names.index("sec-ch-ua") > second.names.index("accept")


def test_a_critical_hint_that_went_starts_nothing_over(servers: Servers) -> None:
    url = servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        asks(servers, session)
        session.get(url)

    assert len(servers.received) == 2


def test_a_critical_hint_that_accept_ch_does_not_name_starts_nothing_over(servers: Servers) -> None:
    with new_session() as session:
        session.get(servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR_TOO))
        session.get(servers.https(critical_ch=ASKED_FOR))

    assert len(servers.received) == 2


@pytest.mark.parametrize("hint", ["sec-ch-ua", "width"])
def test_a_critical_hint_with_nothing_to_add_starts_nothing_over(servers: Servers, hint: str) -> None:
    with new_session() as session:
        session.get(servers.https(accept_ch=hint, critical_ch=hint))

    assert len(servers.received) == 1


def test_a_critical_hint_starts_over_once_for_each_origin(servers: Servers) -> None:
    # What the second start is answered asks for another critical hint, which would start it over again.
    url = servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR, when_sent=ASKED_FOR, then=ASKED_FOR_TOO)
    with new_session() as session:
        session.get(url)
        session.get(servers.https())

    first, second, third = servers.received
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in second.hints
    assert ASKED_FOR not in third.hints
    assert ASKED_FOR_TOO in third.hints


def test_a_critical_hint_on_a_later_hop_starts_over_from_the_first_url(servers: Servers) -> None:
    last = servers.https(CROSS_SITE, "/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    first = redirect(servers.https(HOST, "/first"), last)
    with new_session() as session:
        response = session.get(first)

    assert [received.path.partition("?")[0] for received in servers.received] == [
        "/first",
        "/last",
        "/first",
        "/last",
    ]
    assert servers.received[2].hints == SENT_BY_DEFAULT
    assert ASKED_FOR in servers.received[3].hints
    assert response.status == 200


def test_a_navigation_that_starts_with_a_post_does_not_start_over_for_a_critical_hint(servers: Servers) -> None:
    url = servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        session.post(url, kind="navigation", data={"a": "b"})
        session.get(servers.https())

    first, second = servers.received
    assert (first.method, first.body) == ("POST", b"a=b")
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in second.hints


@pytest.mark.parametrize("status", ["302", "303", "307"])
def test_a_navigation_that_started_with_a_post_does_not_start_over_on_a_later_hop(
    servers: Servers, status: str
) -> None:
    last = servers.https(path="/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        session.post(redirect(servers.https(path="/first"), last, status=status), kind="navigation", data={"a": "b"})

    assert [received.path.partition("?")[0] for received in servers.received] == ["/first", "/last"]


def test_a_later_hop_keeps_the_place_of_the_hints_of_a_restart_and_adds_its_own_after_them(servers: Servers) -> None:
    last = servers.https(CROSS_SITE, "/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        session.get(redirect(servers.https(HOST, "/first"), last))

    assert servers.received[3].hints == ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform", ASKED_FOR]


def test_the_hop_after_a_restart_also_gets_what_the_first_url_had_and_the_hop_after_that_does_not(
    servers: Servers,
) -> None:
    # As real Chrome does: `model` goes to an origin that only asked for `arch`, once.
    end = servers.https(CROSS_SITE, "/end")
    last = servers.https(
        CROSS_SITE, "/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR, when_sent=ASKED_FOR, then_to=end
    )
    with new_session() as session:
        asks(servers, session, HOST, hints=ASKED_FOR_TOO)
        session.get(redirect(servers.https(HOST, "/first"), last))

    restart, next_hop, hop_after = servers.received[3:]
    assert restart.hints == ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform", ASKED_FOR_TOO]
    assert next_hop.hints == ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform", ASKED_FOR_TOO, ASKED_FOR]
    assert hop_after.hints == ["sec-ch-ua", "sec-ch-ua-mobile", ASKED_FOR, "sec-ch-ua-platform"]


def test_a_restart_at_plain_http_sends_what_the_first_request_there_did(servers: Servers) -> None:
    last = servers.https(path="/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        session.get(redirect(servers.http(path="/first"), last))

    first, restart = servers.plain.received
    assert restart.names == first.names
    assert not [name for name in restart.names if name.startswith(("sec-fetch-", "sec-ch-"))]
    assert ASKED_FOR not in servers.received[0].hints
    assert ASKED_FOR in servers.received[1].hints
    assert servers.received[1].header("Sec-Fetch-Site") == "none"


def test_a_restart_for_a_critical_hint_keeps_the_referer_and_forgets_where_the_chain_has_been(servers: Servers) -> None:
    page = servers.https(HOST, "/page")
    last = servers.https(CROSS_SITE, "/last", accept_ch=ASKED_FOR, critical_ch=ASKED_FOR)
    with new_session() as session:
        session.get(redirect(servers.https(HOST, "/first"), last), initiator=page)

    first, second, restart, again = servers.received
    assert (first.header("Sec-Fetch-Site"), first.header("Referer")) == ("same-origin", page)
    assert (second.header("Sec-Fetch-Site"), second.header("Referer")) == ("cross-site", servers.https(HOST))
    assert (restart.header("Sec-Fetch-Site"), restart.header("Referer")) == ("same-origin", servers.https(HOST))
    assert (again.header("Sec-Fetch-Site"), again.header("Referer")) == ("cross-site", servers.https(HOST))


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_critical_hint_starts_no_subresource_over(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session() as session:
        session.get(servers.https(accept_ch=ASKED_FOR, critical_ch=ASKED_FOR), kind=kind)

    assert len(servers.received) == 1


# ACCEPT_CH in ALPS


def alps_for(servers: Servers, host: str = HOST, hints: str = ASKED_FOR) -> dict[str, str]:
    return {f"https://{host}:{servers.secure.server_port}": hints}


def test_alps_makes_a_navigation_send_the_hints_with_nothing_on_the_wire_before_it(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        response = session.get(servers.https())

    assert len(servers.received) == 1
    received = servers.received[0]
    assert response.status == 200
    assert received.header(ASKED_FOR) == '"x86"'


def test_alps_adds_its_hints_after_the_others_and_before_the_fetch_metadata(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.get(servers.https())

    names = servers.received[0].names
    assert names.index("sec-ch-ua") < names.index("accept") < names.index(ASKED_FOR) < names.index("sec-fetch-site")


def test_alps_says_nothing_about_later_requests(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.get(servers.https())
        session.get(servers.https(), kind="fetch")

    assert sorted(servers.received[1].hints) == SENT_BY_DEFAULT


def test_alps_counts_for_each_navigation_of_its_origin(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.get(servers.https())
        session.get(servers.https())

    assert all(ASKED_FOR in received.hints for received in servers.received)


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_alps_says_nothing_to_a_subresource(servers: Servers, kind: mitto.RequestKind) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.get(servers.https(), kind=kind)

    assert sorted(servers.received[0].hints) == SENT_BY_DEFAULT


def test_alps_counts_at_the_hop_it_is_for_and_keeps_where_the_chain_has_been(servers: Servers) -> None:
    page = servers.https(HOST, "/page")
    with new_session(alps=alps_for(servers, CROSS_SITE)) as session:
        session.get(chain(servers.https(HOST), servers.https(CROSS_SITE), servers.https(HOST, "/last")), initiator=page)

    first, second, third = servers.received
    assert ASKED_FOR not in first.hints
    assert ASKED_FOR in second.hints
    assert ASKED_FOR not in third.hints
    assert second.header("Sec-Fetch-Site") == "cross-site"
    assert second.header("Referer") == servers.https(HOST)
    assert third.header("Sec-Fetch-Site") == "cross-site"
    assert third.header("Referer") == servers.https(HOST)


def test_alps_sends_the_body_with_the_request_that_goes_out(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.post(servers.https(), kind="navigation", data={"a": "b"})

    received = servers.received[0]
    assert (received.method, received.body) == ("POST", b"a=b")
    assert received.header(ASKED_FOR) == '"x86"'


def test_alps_that_asks_for_nothing_new_changes_nothing(servers: Servers) -> None:
    with new_session(alps=alps_for(servers, hints="sec-ch-ua, no-such-hint")) as session:
        session.get(servers.https())

    received = servers.received[0]
    assert received.hints == ["sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"]


def test_alps_that_is_not_a_list_of_tokens_changes_nothing(servers: Servers) -> None:
    with new_session(alps=alps_for(servers, hints='"sec-ch-ua-arch"')) as session:
        session.get(servers.https())

    assert sorted(servers.received[0].hints) == SENT_BY_DEFAULT


def test_alps_cannot_keep_a_navigation_going_for_ever(servers: Servers) -> None:
    hosts = [f"host{number}.example.test" for number in range(25)]
    alps = {f"https://{host}:{servers.secure.server_port}": ASKED_FOR for host in hosts}
    with new_session(alps=alps) as session, pytest.raises(RequestError) as caught:
        session.get(chain(*(servers.https(host) for host in hosts)))

    assert caught.value.net_error == ERR_TOO_MANY_ACCEPT_CH_RESTARTS


# A body that cannot go twice


def test_a_stream_goes_on_with_what_it_has_for_alps(servers: Servers) -> None:
    with new_session(alps=alps_for(servers)) as session:
        session.post(servers.https(), kind="navigation", data=iter([b"a=b"]), headers={"Content-Length": "3"})

    assert len(servers.received) == 1
    assert servers.received[0].body == b"a=b"
    assert ASKED_FOR not in servers.received[0].hints


# The Machine


def machine_hints(received: Received) -> dict[str, str | None]:
    return {name: received.header(name) for name in sorted(MACHINE_HINTS)}


def chromes_machine_hints(
    machine: mitto.Machine, host: str = HOST, kind: mitto.RequestKind = mitto.RequestKind.NAVIGATION
) -> dict[str, str | None]:
    """Return what Chrome on `machine`, with the salts a Session draws for it, sends on a hop to `host` of the hints that
    follow the machine: rtt and downlink from the browser process for a navigation, and from the renderer process of the
    page's site for a subresource, whose page is at `host`."""
    salts = draw_noise_salts(machine)
    values: dict[str, str | None] = dict(client_hints(machine))
    if kind is mitto.RequestKind.NAVIGATION:
        values["rtt"] = browser_rtt(host, machine.round_trip_ms, salts.browser)
        values["downlink"] = browser_downlink(host, machine.throughput_kbps, salts.browser)
    else:
        salt = renderer_salt(salts.renderer_key, site_of(host))
        values["rtt"] = renderer_rtt(host, machine.round_trip_ms, salt)
        values["downlink"] = renderer_downlink(host, machine.throughput_kbps, salt)
    return dict(sorted(values.items()))


def site_of(host: str) -> str:
    """Return the site of a page at `host`, as SchemefulSite writes it, for a host whose last label is a registry of one
    label, known or not, as `test` is to Chrome."""
    return "https://" + ".".join(host.split(".")[-2:])


@pytest.mark.parametrize("kind", ALL_KINDS)
def test_a_session_with_no_machine_sends_the_values_of_the_machine_drawn_for_it(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    with new_session() as session:
        page = asks(servers, session, hints=EVERY_HINT)
        session.get(servers.https(), kind=kind, initiator=page)
        machine = session.machine

    assert machine.seed is not None
    assert machine_hints(servers.received[1]) == chromes_machine_hints(machine, kind=kind)


def test_two_sessions_with_no_machine_draw_one_each(servers: Servers) -> None:
    with new_session() as first, new_session() as second:
        assert first.machine.seed != second.machine.seed


def test_a_session_with_a_machine_of_the_same_seed_sends_the_same_values(servers: Servers) -> None:
    for _ in range(2):
        with new_session(machine=mitto.Machine.draw(seed=7)) as session:
            asks(servers, session, hints=EVERY_HINT)
            session.get(servers.https())

    assert machine_hints(servers.received[1]) == machine_hints(servers.received[3])
    assert machine_hints(servers.received[1]) == chromes_machine_hints(mitto.Machine.draw(seed=7))


def test_a_session_keeps_a_value_that_its_machine_fixes(servers: Servers) -> None:
    machine = mitto.Machine.draw(seed=7, memory=32, color_scheme="dark", platform_version="15.0.0")
    with new_session(machine=machine) as session:
        asks(servers, session, hints=EVERY_HINT)
        session.get(servers.https())
        assert session.machine is machine

    received = servers.received[1]
    assert received.header("device-memory") == received.header("sec-ch-device-memory") == "32"
    assert received.header("sec-ch-prefers-color-scheme") == "dark"
    assert received.header("sec-ch-ua-platform-version") == '"15.0.0"'


def test_a_session_sends_the_values_of_a_machine_on_each_hop(servers: Servers) -> None:
    machine = mitto.Machine.draw(seed=8)
    with new_session(machine=machine) as session:
        asks(servers, session, hints=EVERY_HINT)
        session.get(chain(servers.https(path="/a"), servers.https(path="/b")))

    assert [machine_hints(received) for received in servers.received[1:]] == [chromes_machine_hints(machine)] * 2


def test_the_machine_of_mitto_0_1_sends_what_mitto_0_1_sent_but_for_the_noise_on_downlink(servers: Servers) -> None:
    with new_session(machine=mitto.Machine.named("mitto-0.1")) as session:
        asks(servers, session, hints=EVERY_HINT)
        session.get(servers.https())

    # It has no seed, so its salts are drawn at random, and the noise can take the downlink below 10.
    downlinks = {browser_downlink(HOST, 10_000, salt) for salt in range(1, 22)}
    assert servers.received[1].header("downlink") in downlinks
    assert machine_hints(servers.received[1]) == {
        "device-memory": "8",
        "downlink": servers.received[1].header("downlink"),
        "dpr": "1",
        "ect": "4g",
        "rtt": "50",
        "sec-ch-device-memory": "8",
        "sec-ch-dpr": "1",
        "sec-ch-prefers-color-scheme": "light",
        "sec-ch-prefers-reduced-motion": "no-preference",
        "sec-ch-prefers-reduced-transparency": "no-preference",
        "sec-ch-ua-platform-version": '"19.0.0"',
        "sec-ch-viewport-height": "945",
        "sec-ch-viewport-width": "1920",
        "viewport-width": "1920",
    }


def test_a_session_saved_and_loaded_in_another_process_sends_the_same_values(
    servers: Servers, tmp_path: Path, both_apis: str
) -> None:
    cookie_file = tmp_path / "cookies.json"
    with new_session(cookie_file=cookie_file) as session:
        session.cookies.set(mitto.Cookie("a", "1", domain=HOST))
        page = asks(servers, session, hints=EVERY_HINT)
        session.get(servers.https())
        machine = session.machine

    claimed = subprocess.run(
        [
            sys.executable,
            SAVED_SESSION_CLIENT,
            cookie_file,
            PRIVATE_CERTIFICATE_SPKI_HASH,
            page,
            servers.https(),
            *(["--asyncio"] if both_apis == "asyncio" else []),
        ],
        capture_output=True,
        text=True,
        check=True,
        timeout=TIMEOUT_SECONDS * 2,
    ).stdout

    assert json.loads(claimed) == dataclasses.asdict(machine)
    first, again = servers.received[1], servers.received[3]
    assert machine_hints(again) == machine_hints(first)
    assert again.headers == first.headers
    assert again.header("cookie") == "a=1"


# rtt and downlink for each host

NETWORK_HINTS = "rtt, downlink"


def network_hints(received: Received) -> tuple[str | None, str | None]:
    return received.header("rtt"), received.header("downlink")


def browsers(machine: mitto.Machine, host: str) -> tuple[str, str]:
    """Return the rtt and downlink that Chrome's browser process sends on a navigation's hop to `host`."""
    return chromes_network_hints(machine, host, mitto.RequestKind.NAVIGATION)


def renderers(machine: mitto.Machine, host: str) -> tuple[str, str]:
    """Return the rtt and downlink that the renderer of a page at `host` sends on a subresource to its own origin."""
    return chromes_network_hints(machine, host, mitto.RequestKind.FETCH)


def chromes_network_hints(machine: mitto.Machine, host: str, kind: mitto.RequestKind) -> tuple[str, str]:
    hints = chromes_machine_hints(machine, host, kind)
    return cast(str, hints["rtt"]), cast(str, hints["downlink"])


def machine_where(holds: Callable[[mitto.Machine], bool]) -> mitto.Machine:
    """Return a Machine with the first seed whose salts make `holds` true of it, which keeps the values of seed 0."""
    machine = mitto.Machine.draw(seed=0)
    return next(
        candidate
        for candidate in (dataclasses.replace(machine, seed=seed) for seed in itertools.count())
        if holds(candidate)
    )


def test_a_redirect_to_another_host_changes_rtt_and_downlink_as_chromes_browser_process_does(
    servers: Servers,
) -> None:
    machine = machine_where(lambda machine: browsers(machine, HOST) != browsers(machine, CROSS_SITE))
    with new_session(machine=machine) as session:
        asks(servers, session, HOST, hints=NETWORK_HINTS)
        asks(servers, session, CROSS_SITE, hints=NETWORK_HINTS)
        session.get(chain(servers.https(HOST, "/first"), servers.https(CROSS_SITE, "/last")))

    first, last = servers.received[2:]
    assert network_hints(first) == browsers(machine, HOST)
    assert network_hints(last) == browsers(machine, CROSS_SITE)


@pytest.mark.parametrize("kind", SUBRESOURCES)
def test_a_subresource_gets_rtt_and_downlink_from_the_renderer_of_its_pages_site(
    servers: Servers, kind: mitto.RequestKind
) -> None:
    machine = machine_where(lambda machine: renderers(machine, HOST) != browsers(machine, HOST))
    with new_session(machine=machine) as session:
        page = asks(servers, session, HOST, hints=NETWORK_HINTS)
        session.get(servers.https(HOST, "/resource"), kind=kind, initiator=page)
        session.get(servers.https(HOST, "/next"))

    resource, navigation = servers.received[1:]
    assert network_hints(resource) == renderers(machine, HOST)
    assert network_hints(navigation) == browsers(machine, HOST)


def test_the_pages_of_each_site_have_a_renderer_with_a_salt_of_its_own(servers: Servers) -> None:
    def salts_differ(machine: mitto.Machine) -> bool:
        key = draw_noise_salts(machine).renderer_key
        return renderer_salt(key, site_of(HOST)) != renderer_salt(key, site_of(CROSS_SITE))

    machine = machine_where(salts_differ)
    with new_session(machine=machine) as session:
        for host in (HOST, SAME_SITE, CROSS_SITE):
            page = asks(servers, session, host, hints=NETWORK_HINTS)
            session.get(servers.https(host, "/resource"), kind=mitto.RequestKind.FETCH, initiator=page)

    assert [network_hints(received) for received in servers.received[1::2]] == [
        renderers(machine, host) for host in (HOST, SAME_SITE, CROSS_SITE)
    ]


@pytest.mark.parametrize("next_origin_asked", [False, True], ids=["next-origin-did-not-ask", "next-origin-asked"])
def test_the_hop_after_a_restart_keeps_the_restarts_rtt_where_its_origin_did_not_ask_and_has_its_own_where_it_did(
    servers: Servers, next_origin_asked: bool
) -> None:
    # The response that starts the navigation over says what its origin asks for from then on.
    machine = machine_where(lambda machine: browsers(machine, HOST) != browsers(machine, CROSS_SITE))
    end = servers.https(CROSS_SITE, "/end")
    accept_ch = f"{ASKED_FOR}, {NETWORK_HINTS}" if next_origin_asked else ASKED_FOR
    last = servers.https(
        CROSS_SITE, "/last", accept_ch=accept_ch, critical_ch=ASKED_FOR, when_sent=ASKED_FOR, then_to=end
    )
    with new_session(machine=machine) as session:
        asks(servers, session, HOST, hints=NETWORK_HINTS)
        session.get(redirect(servers.https(HOST, "/first"), last))

    restart, next_hop, hop_after = servers.received[-3:]
    assert network_hints(restart) == browsers(machine, HOST)
    if next_origin_asked:
        assert network_hints(next_hop) == network_hints(hop_after) == browsers(machine, CROSS_SITE)
    else:
        assert network_hints(next_hop) == browsers(machine, HOST)
        assert network_hints(hop_after) == (None, None)


def test_a_restart_for_alps_works_them_out_for_its_own_url(servers: Servers) -> None:
    machine = mitto.Machine.draw(seed=9)
    with new_session(alps=alps_for(servers, CROSS_SITE, NETWORK_HINTS), machine=machine) as session:
        session.get(servers.https(CROSS_SITE))

    (received,) = servers.received
    assert network_hints(received) == browsers(machine, CROSS_SITE)


def test_a_callers_rtt_and_downlink_go_as_given_on_each_hop(servers: Servers) -> None:
    with new_session(machine=mitto.Machine.draw(seed=9)) as session:
        asks(servers, session, HOST, hints=NETWORK_HINTS)
        asks(servers, session, CROSS_SITE, hints=NETWORK_HINTS)
        session.get(
            chain(servers.https(HOST, "/first"), servers.https(CROSS_SITE, "/last")),
            headers={"RTT": "1000", "Downlink": "0.5"},
        )

    assert [network_hints(received) for received in servers.received[2:]] == [("1000", "0.5")] * 2


# The cases of content/browser/client_hints/client_hints_unittest.cc, which call RoundRtt() and RoundKbpsToMbps() of the
# browser process with an estimate and a host. Each estimate here is a Machine's, so a Session claims each pair of a round
# trip and a throughput. Some cases have an empty host, which no URL has, so each estimate goes to the two hosts of the
# others, and to a script, image or fetch as well, whose renderer has the same rounding.

CHROMIUM_ESTIMATES = [
    (12, 12),
    (98, 98),
    (1023, 1023),
    (1193, 1193),
    (2750, 7523),
    (2789, 9999),
    (6023, 100102),
    (6787, 12102),
    (1023, 102),
    (1023, 2102),
]
CHROMIUM_HOSTS = ["foo.com", "example.com"]

# How far the noise and the rounding can take each value from the estimate, in ms and Mbps, where the cases say.
RTT_WITHIN = {98: 100, 1023: 200, 1193: 200, 2750: 400}
DOWNLINK_WITHIN = {98: 0.1, 1023: 0.2, 1193: 0.2, 7523: 0.9, 9999: 1.2}


@dataclass(frozen=True, slots=True)
class Sent:
    host: str
    kind: mitto.RequestKind
    rtt: str
    downlink: str


def network_hints_of_each_kind(servers: Servers, machine: mitto.Machine, hosts: list[str]) -> list[Sent]:
    """Return what a Session sends as rtt and downlink to each host in turn, which has asked, on a navigation and on a
    fetch from a page there."""
    sent: list[Sent] = []
    with new_session(machine=machine) as session:
        for host in hosts:
            page = asks(servers, session, host, hints=NETWORK_HINTS)
            for kind in (mitto.RequestKind.NAVIGATION, mitto.RequestKind.FETCH):
                session.get(servers.https(host, "/next"), kind=kind, initiator=page)
                rtt, downlink = network_hints(servers.received[-1])
                sent.append(Sent(host, kind, cast(str, rtt), cast(str, downlink)))
    return sent


@pytest.mark.parametrize(("round_trip_ms", "throughput_kbps"), CHROMIUM_ESTIMATES)
def test_rtt_and_downlink_keep_to_the_rounding_and_the_cap_of_chromiums_cases(
    servers: Servers, round_trip_ms: int, throughput_kbps: int
) -> None:
    machine = mitto.Machine.draw(seed=10, round_trip_ms=round_trip_ms, throughput_kbps=throughput_kbps)
    sent = network_hints_of_each_kind(servers, machine, CHROMIUM_HOSTS)

    for one in sent:
        assert (one.rtt, one.downlink) == chromes_network_hints(machine, one.host, one.kind)
        rtt = int(one.rtt)
        downlink = float(one.downlink)
        assert rtt % 50 == 0
        assert rtt <= 3000
        assert round(downlink * 1000) % 50 == 0
        assert downlink <= 10
        if round_trip_ms in RTT_WITHIN:
            assert abs(rtt - round_trip_ms) <= RTT_WITHIN[round_trip_ms]
        if throughput_kbps in DOWNLINK_WITHIN:
            assert abs(downlink - throughput_kbps / 1000) <= DOWNLINK_WITHIN[throughput_kbps]


def test_rtt_and_downlink_change_with_the_host_and_stay_for_one_host(servers: Servers) -> None:
    # One in 21 hosts lands on the multiplier of another, so 20 others are all but sure to have one that does not.
    machine = mitto.Machine.draw(seed=10, round_trip_ms=1023, throughput_kbps=1023)
    hosts = ["example.com", *(f"{number}.test" for number in range(20)), "example.com"]
    sent = network_hints_of_each_kind(servers, machine, hosts)

    for kind in (mitto.RequestKind.NAVIGATION, mitto.RequestKind.FETCH):
        of_kind = [one for one in sent if one.kind is kind]
        first, others, again = of_kind[0], of_kind[1:-1], of_kind[-1]
        assert any(one.rtt != first.rtt for one in others)
        assert any(one.downlink != first.downlink for one in others)
        assert (again.rtt, again.downlink) == (first.rtt, first.downlink)


def test_a_host_of_each_length_that_the_hashes_tell_apart_gets_chromes_values(servers: Servers) -> None:
    # Blink's rapidhash reads up to 3, up to 16, up to 48 and more bytes each in a way of its own, and libc++'s CityHash64
    # up to 3, up to 8, up to 16, up to 32, up to 64 and more.
    lengths = [1, 2, 3, 4, 5, 8, 9, 12, 16, 17, 24, 32, 33, 48, 49, 64, 65, 97, 128, 129, 200]
    hosts = [_host_of_length(length) for length in lengths]
    machine = mitto.Machine.draw(seed=11, round_trip_ms=1023, throughput_kbps=7523)
    sent = network_hints_of_each_kind(servers, machine, hosts)

    assert [one.host for one in sent] == [host for host in hosts for _ in range(2)]
    for one in sent:
        assert (one.rtt, one.downlink) == chromes_network_hints(machine, one.host, one.kind), (len(one.host), one.kind)


def _host_of_length(length: int) -> str:
    """Return a host name of `length` letters and dots, with a dot after each 60 letters."""
    host = ""
    while len(host) < length:
        host += "." if len(host) % 61 == 60 else chr(ord("a") + len(host) % 26)
    assert not host.endswith(".")
    return host
