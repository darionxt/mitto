"""Tests for how a Session verifies certificates, through the real native core to servers on loopback.

The chains are private and made fresh for each test, so nothing trusts them until a test adds its root. The tests need no
network connection, and are skipped when this machine has no native library.
"""

import datetime
import http.server
import time
from pathlib import Path

import pytest

import mitto
from mitto import _native
from mitto._native import Finished, PrivateSettings, RequestError
from tests.local_certificates import (
    ISSUER_PATH,
    AiaServer,
    TlsServer,
    generate_intermediate,
    generate_leaf,
    generate_root,
    serving,
)
from tools.capture import HTTP1, HTTP2, HTTP3, CaptureServer

pytestmark = pytest.mark.usefixtures("native_library")

HOST = "localhost"
ERR_ABORTED = -3
ERR_CERT_AUTHORITY_INVALID = -202

# Longer than a request to loopback takes, and shorter than the fetcher's own timeout of 15 seconds.
FETCH_TIMEOUT_SECONDS = 10

# How long a Session may take to try QUIC once Alt-Svc has advertised it. A slow CI runner needs more than a local build.
ALT_SVC_TIMEOUT_SECONDS = 30

# How long a Session gets to finish a QUIC handshake that the server has seen, so that a later request could use it.
QUIC_HANDSHAKE_SECONDS = 0.5


def url_of(server: http.server.HTTPServer) -> str:
    return f"https://{HOST}:{server.server_port}/"


def test_a_private_chain_is_refused_without_its_anchor(tmp_path: Path) -> None:
    root = generate_root()
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session() as session,
        pytest.raises(RequestError) as caught,
    ):
        session.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_a_private_chain_is_accepted_with_its_anchor(tmp_path: Path) -> None:
    root = generate_root()
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
    ):
        response = session.get(url_of(server))

    assert response.status == 200


def test_a_private_chain_is_refused_with_another_anchor(tmp_path: Path) -> None:
    root = generate_root()
    other = generate_root("another mitto test root")
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session(additional_trust_anchors=[other.pem]) as session,
        pytest.raises(RequestError) as caught,
    ):
        session.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_an_anchor_holds_only_for_the_session_that_added_it(tmp_path: Path) -> None:
    root = generate_root()
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as adding,
        mitto.Session() as other,
    ):
        assert adding.get(url_of(server)).status == 200
        with pytest.raises(RequestError) as caught:
            other.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_an_entry_may_hold_several_anchors_and_other_text(tmp_path: Path) -> None:
    root = generate_root()
    other = generate_root("another mitto test root")
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session(additional_trust_anchors=[f"# Two roots\n{other.pem}between them\n{root.pem}"]) as session,
    ):
        response = session.get(url_of(server))

    assert response.status == 200


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_a_self_signed_certificate_can_be_its_own_anchor(protocol: str) -> None:
    with CaptureServer() as server, mitto.Session(additional_trust_anchors=[server.certificate_pem]) as session:
        response = session.get(server.url(protocol))

    assert response.protocol == protocol


def test_an_expired_anchor_still_counts(tmp_path: Path) -> None:
    root = generate_root(validity=datetime.timedelta(days=-1))
    with (
        serving(TlsServer(tmp_path, generate_leaf(root, HOST))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
    ):
        response = session.get(url_of(server))

    assert response.status == 200


def test_a_chain_with_its_intermediate_verifies_with_no_fetch(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(intermediate.der)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url), intermediate)) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
    ):
        response = session.get(url_of(server))

    assert response.status == 200
    assert aia.requests == []


def test_a_missing_intermediate_is_fetched_from_its_aia_url(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(intermediate.der)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
    ):
        response = session.get(url_of(server))

    assert response.status == 200
    assert [path for path, _ in aia.requests] == [ISSUER_PATH]


def test_a_certificate_fetch_sends_the_lines_chrome_sends(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(intermediate.der)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
    ):
        session.get(url_of(server))

    # Recorded from Chrome 156 on macOS, with no cookies: //net builds this request, so the order is its own.
    assert [name for name, _ in aia.requests[0][1]] == [
        "Host",
        "Connection",
        "User-Agent",
        "Accept-Encoding",
        "Accept-Language",
    ]


def test_a_missing_intermediate_with_no_aia_url_is_refused(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
        pytest.raises(RequestError) as caught,
    ):
        session.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID


def test_a_missing_intermediate_whose_aia_url_fails_is_refused(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(None)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
        mitto.Session(additional_trust_anchors=[root.pem]) as session,
        pytest.raises(RequestError) as caught,
    ):
        session.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID
    assert [path for path, _ in aia.requests] == [ISSUER_PATH]


def test_a_fetched_intermediate_does_not_make_a_chain_trusted(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(intermediate.der)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
        mitto.Session() as session,
        pytest.raises(RequestError) as caught,
    ):
        session.get(url_of(server))

    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID
    assert [path for path, _ in aia.requests] == [ISSUER_PATH]


def finished_event(request: _native.NativeRequest) -> Finished:
    while True:
        event = request.next_event(FETCH_TIMEOUT_SECONDS)
        assert event is not None, "the request did not end in time"
        if isinstance(event, Finished):
            return event


def test_closing_a_session_ends_a_certificate_fetch(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(hang=True)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
    ):
        session = _native.Session(additional_trust_anchors=[root.pem])
        with session.start(url_of(server)) as request:
            assert aia.requested.wait(FETCH_TIMEOUT_SECONDS)
            session.close()
            finished = finished_event(request)
        aia.released.set()

    assert finished.net_error == ERR_ABORTED


def test_a_session_closes_with_the_fetch_of_a_request_that_was_cancelled(tmp_path: Path) -> None:
    root = generate_root()
    intermediate = generate_intermediate(root)
    with (
        serving(AiaServer(hang=True)) as aia,
        serving(TlsServer(tmp_path, generate_leaf(intermediate, HOST, aia.url))) as server,
    ):
        session = _native.Session(additional_trust_anchors=[root.pem])
        with session.start(url_of(server)) as request:
            assert aia.requested.wait(FETCH_TIMEOUT_SECONDS)
            request.cancel()
            finished = finished_event(request)
        session.close()
        aia.released.set()

    assert finished.net_error == ERR_ABORTED


def test_quic_is_refused_to_an_added_root(tmp_path: Path) -> None:
    root = generate_root()
    with (
        CaptureServer() as quic_server,
        serving(TlsServer(tmp_path, generate_leaf(root, quic_server.host))) as tcp_server,
        mitto.Session(additional_trust_anchors=[root.pem, quic_server.certificate_pem]) as session,
    ):
        tcp_server.response_headers = [("Alt-Svc", f'h3=":{quic_server.port(HTTP3)}"')]
        url = url_of(tcp_server).replace(HOST, quic_server.host)
        protocols = {session.get(url).protocol}

        deadline = time.monotonic() + ALT_SVC_TIMEOUT_SECONDS
        while not quic_server.recording(HTTP3).connections and time.monotonic() < deadline:
            protocols.add(session.get(url).protocol)
        assert quic_server.recording(HTTP3).connections, "the Session never tried QUIC"

        deadline = time.monotonic() + QUIC_HANDSHAKE_SECONDS
        while time.monotonic() < deadline:
            protocols.add(session.get(url).protocol)

    assert protocols == {"http/1.1"}


def test_forcing_quic_on_a_host_lifts_the_refusal_of_an_added_root(tmp_path: Path) -> None:
    root = generate_root()
    with (
        CaptureServer() as quic_server,
        serving(TlsServer(tmp_path, generate_leaf(root, quic_server.host))) as tcp_server,
        mitto.Session(
            additional_trust_anchors=[root.pem, quic_server.certificate_pem],
            # The origin here is the TCP one, so this forces nothing. It only lifts the refusal for the host.
            _settings=PrivateSettings(origins_to_force_quic_on=[quic_server.authority(HTTP3)]),
        ) as session,
    ):
        tcp_server.response_headers = [("Alt-Svc", f'h3=":{quic_server.port(HTTP3)}"')]
        url = url_of(tcp_server).replace(HOST, quic_server.host)
        protocol = session.get(url).protocol
        deadline = time.monotonic() + ALT_SVC_TIMEOUT_SECONDS
        while protocol != "h3" and time.monotonic() < deadline:
            time.sleep(0.01)
            protocol = session.get(url).protocol

    assert protocol == "h3"


@pytest.mark.parametrize(
    "anchors",
    [
        pytest.param(["not a certificate"], id="no-pem-block"),
        pytest.param([""], id="empty"),
        pytest.param(["-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"], id="another-pem-type"),
        pytest.param(["-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"], id="not-a-valid-certificate"),
    ],
)
def test_anchors_that_cannot_be_read_are_refused(anchors: list[str]) -> None:
    with pytest.raises(ValueError, match="refused the settings"):
        mitto.Session(additional_trust_anchors=anchors)


def test_one_anchor_that_cannot_be_read_refuses_the_rest() -> None:
    with pytest.raises(ValueError, match="refused the settings"):
        mitto.Session(additional_trust_anchors=[generate_root().pem, "not a certificate"])
