"""Tests for how a Session resolves names, through the real native core to a DNS server and a capture server on loopback.

Names under .test are never looked up anywhere else, because each of these Sessions asks only its own DNS server. So the
tests need no network connection, and are skipped when this machine has no native library.
"""

import dataclasses
from collections.abc import Iterator

import pytest

from mitto._native import Session
from tests.local_dns_server import (
    ECH_AEAD_AES_128_GCM,
    ECH_KDF_HKDF_SHA256,
    TYPE_A,
    TYPE_HTTPS,
    DnsQuery,
    LocalDnsServer,
    ech_config_list,
    https_record,
)
from tools.capture import HTTP2, HTTP3, CaptureServer
from tools.capture.client_hello import ClientHello
from tools.capture.recording import ConnectionRecord
from tools.capture.wire import Reader

pytestmark = pytest.mark.usefixtures("native_library")

HOST = "site.test"
PUBLIC_NAME = "public.test"
ECH_CONFIG_ID = 42

_EXTENSION_SERVER_NAME = 0
_EXTENSION_ENCRYPTED_CLIENT_HELLO = 65037
_ECH_OUTER = 0


@dataclasses.dataclass(frozen=True, slots=True)
class OuterEch:
    """What a ClientHello's encrypted_client_hello extension says in the clear."""

    kdf: int
    aead: int
    config_id: int


def extension(hello: ClientHello, extension_type: int) -> Reader:
    return next(Reader(item.data) for item in hello.extensions if item.type == extension_type)


def server_name(hello: ClientHello) -> str:
    names = extension(hello, _EXTENSION_SERVER_NAME).vector(2)
    names.uint(1)
    return names.opaque(2).decode()


def outer_ech(hello: ClientHello) -> OuterEch:
    reader = extension(hello, _EXTENSION_ENCRYPTED_CLIENT_HELLO)
    assert reader.uint(1) == _ECH_OUTER
    return OuterEch(kdf=reader.uint(2), aead=reader.uint(2), config_id=reader.uint(1))


def client_hellos(server: CaptureServer) -> list[ClientHello]:
    connections = server.recording(HTTP2).connections
    assert all(isinstance(connection, ConnectionRecord) for connection in connections)
    return [connection.client_hello.hello for connection in connections if isinstance(connection, ConnectionRecord)]


@pytest.fixture
def capture_server() -> Iterator[CaptureServer]:
    with CaptureServer() as server:
        yield server


def test_an_https_record_with_ech_puts_real_ech_in_the_client_hello(capture_server: CaptureServer) -> None:
    record = https_record(HOST, alpn=[HTTP2], ech=ech_config_list(ECH_CONFIG_ID, PUBLIC_NAME))

    with (
        LocalDnsServer(record) as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        response = session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    first, retry = client_hellos(capture_server)
    assert server_name(first) == PUBLIC_NAME
    assert outer_ech(first) == OuterEch(ECH_KDF_HKDF_SHA256, ECH_AEAD_AES_128_GCM, ECH_CONFIG_ID)
    # The capture server knows no ECH, so it completes the handshake for the public name and offers no new configuration.
    # The client then connects again in the clear, with what it sends to a host that has no configuration.
    assert server_name(retry) == HOST
    assert response.status == 200


def test_an_https_record_without_ech_leaves_ech_grease_in_the_client_hello(capture_server: CaptureServer) -> None:
    with (
        LocalDnsServer(https_record(HOST, alpn=[HTTP2])) as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        response = session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    (hello,) = client_hellos(capture_server)
    assert server_name(hello) == HOST
    assert outer_ech(hello).kdf == ECH_KDF_HKDF_SHA256
    assert response.status == 200


def test_no_https_record_leaves_ech_grease_in_the_client_hello(capture_server: CaptureServer) -> None:
    with (
        LocalDnsServer() as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        response = session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    (hello,) = client_hellos(capture_server)
    assert server_name(hello) == HOST
    assert response.status == 200


def test_a_session_asks_for_the_address_and_the_https_record(capture_server: CaptureServer) -> None:
    port = capture_server.port(HTTP2)

    with (
        LocalDnsServer() as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        session.request(f"https://{HOST}:{port}/")

    assert DnsQuery(HOST, TYPE_A) in dns.queries
    assert DnsQuery(f"_{port}._https.{HOST}", TYPE_HTTPS) in dns.queries


def test_a_session_asks_only_its_own_dns_server(capture_server: CaptureServer) -> None:
    with (
        LocalDnsServer() as asked,
        LocalDnsServer() as other,
        Session(
            ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=asked.address
        ) as session,
        Session(dns_nameserver=other.address),
    ):
        session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    assert asked.queries
    assert not other.queries


def test_an_h3_https_record_brings_a_first_request_over_h3(capture_server: CaptureServer) -> None:
    with (
        LocalDnsServer(https_record(HOST, alpn=[HTTP3])) as dns,
        # This port takes no requests, so it forces nothing. It only lifts //net's refusal of QUIC to a private
        # certificate for the host, which a public server does not need.
        Session(
            ignore_certificate_errors_spki_list=[capture_server.spki_hash],
            origins_to_force_quic_on=[f"{HOST}:1"],
            dns_nameserver=dns.address,
        ) as session,
    ):
        response = session.request(f"https://{HOST}:{capture_server.port(HTTP3)}/")

    assert response.protocol == HTTP3
