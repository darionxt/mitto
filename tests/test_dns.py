"""Tests for how a Session resolves names, through the real native core to a DNS server and a capture server on loopback.

Names under .test are never looked up anywhere else, because each of these Sessions asks only its own DNS server. So the
tests need no network connection, and are skipped when this machine has no native library.
"""

import dataclasses
from collections.abc import Iterator
from typing import cast

import pytest

from mitto import RequestError
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
from tests.local_ech import EchKey
from tools.capture import HTTP2, HTTP3, CaptureServer
from tools.capture.client_hello import ClientHello
from tools.capture.normal_form import normal_hello
from tools.capture.recording import ConnectionRecord, Json, QuicConnectionRecord
from tools.capture.wire import Reader

pytestmark = pytest.mark.usefixtures("native_library")

HOST = "site.test"
PUBLIC_NAME = "public.test"
ECH_CONFIG_ID = 42

_EXTENSION_SERVER_NAME = 0
_EXTENSION_ENCRYPTED_CLIENT_HELLO = 65037
_ECH_OUTER = 0

# What Chrome 157 left out of its ClientHelloInner over TCP on 2026-11-07, next to its outer ClientHello: all that is
# for TLS 1.2 only. See docs/capture.md.
_TLS_1_2_EXTENSIONS = frozenset({"ec_point_formats", "extended_master_secret", "renegotiation_info", "session_ticket"})
_TLS_1_3_CIPHER_SUITES = frozenset(
    {"GREASE", "TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384", "TLS_CHACHA20_POLY1305_SHA256"}
)
_INNER_ECH: Json = {"type": "encrypted_client_hello", "client_hello": "inner"}


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
    hellos: list[ClientHello] = []
    for connection in server.recording(HTTP2).connections:
        assert isinstance(connection, ConnectionRecord)
        assert connection.client_hello is not None
        hellos.append(connection.client_hello.hello)
    return hellos


def quic_client_hellos(server: CaptureServer) -> list[ClientHello]:
    hellos: list[ClientHello] = []
    for connection in server.recording(HTTP3).connections:
        assert isinstance(connection, QuicConnectionRecord)
        hellos.append(connection.client_hello.hello)
    return hellos


def inner_form(outer: dict[str, Json], *, tls_1_2: bool) -> dict[str, Json]:
    """Return what the normal form of a ClientHelloInner should be, from that of its outer ClientHello.

    The inner one offers TLS 1.3 only, so it leaves out what the outer one has for TLS 1.2, which over QUIC is nothing.
    """
    extensions: list[Json] = []
    for item in cast(list[dict[str, Json]], outer["extensions"]):
        if item["type"] == "encrypted_client_hello":
            extensions.append(_INNER_ECH)
        elif not tls_1_2:
            extensions.append(item)
        elif item["type"] == "supported_versions":
            extensions.append({**item, "versions": [v for v in cast(list[Json], item["versions"]) if v != "TLS 1.2"]})
        elif item["type"] not in _TLS_1_2_EXTENSIONS:
            extensions.append(item)
    suites = cast(list[Json], outer["cipher_suites"])
    if tls_1_2:
        suites = [suite for suite in suites if suite in _TLS_1_3_CIPHER_SUITES]
    form: dict[str, Json] = {**outer, "cipher_suites": suites, "extensions": extensions}
    return form


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


def test_real_ech_looks_like_ech_grease_from_outside(capture_server: CaptureServer) -> None:
    key = EchKey.generate(ECH_CONFIG_ID, PUBLIC_NAME)

    with (
        LocalDnsServer(https_record(HOST, alpn=[HTTP2], ech=key.config_list)) as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    real, grease = client_hellos(capture_server)
    assert server_name(key.inner_client_hello(real)) == HOST
    with pytest.raises(ValueError, match="another configuration"):
        key.inner_client_hello(grease)
    # Chrome 157 sent the same: with real ECH, its ClientHello had the normal form of its Baselines, which hold GREASE.
    assert normal_hello(real) == normal_hello(grease)


def test_the_client_hello_inner_offers_tls_1_3_only(capture_server: CaptureServer) -> None:
    key = EchKey.generate(ECH_CONFIG_ID, PUBLIC_NAME)

    with (
        LocalDnsServer(https_record(HOST, alpn=[HTTP2], ech=key.config_list)) as dns,
        Session(ignore_certificate_errors_spki_list=[capture_server.spki_hash], dns_nameserver=dns.address) as session,
    ):
        session.request(f"https://{HOST}:{capture_server.port(HTTP2)}/")

    outer, _ = client_hellos(capture_server)
    inner = key.inner_client_hello(outer)
    assert server_name(inner) == HOST
    assert normal_hello(inner) == inner_form(normal_hello(outer), tls_1_2=True)


def test_over_quic_the_client_hello_inner_differs_only_in_its_type(capture_server: CaptureServer) -> None:
    key = EchKey.generate(ECH_CONFIG_ID, PUBLIC_NAME)

    with (
        LocalDnsServer(https_record(HOST, alpn=[HTTP3], ech=key.config_list)) as dns,
        # As in the test of an h3 record below, this only lifts //net's refusal of QUIC to a private certificate.
        Session(
            ignore_certificate_errors_spki_list=[capture_server.spki_hash],
            origins_to_force_quic_on=[f"{HOST}:1"],
            dns_nameserver=dns.address,
        ) as session,
        # The capture server knows no ECH. Over QUIC, //net does not connect again without it, as it does over TCP.
        pytest.raises(RequestError),
    ):
        session.request(f"https://{HOST}:{capture_server.port(HTTP3)}/")

    outer, *_ = quic_client_hellos(capture_server)
    inner = key.inner_client_hello(outer)
    assert server_name(outer) == PUBLIC_NAME
    assert server_name(inner) == HOST
    assert normal_hello(inner) == inner_form(normal_hello(outer), tls_1_2=False)


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
