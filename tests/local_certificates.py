"""Private certificate chains for tests, with a local HTTP server to hold what an AIA URL names."""

import datetime
import http.server
import ssl
import threading
from collections.abc import Generator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import AuthorityInformationAccessOID, ExtendedKeyUsageOID, NameOID

from tests.local_server import QuietServer

_VALIDITY = datetime.timedelta(days=30)
_CLOCK_SKEW = datetime.timedelta(days=1)

ISSUER_PATH = "/issuer.der"


@dataclass(frozen=True, slots=True)
class Issued:
    """A certificate and its key."""

    certificate: x509.Certificate
    key: ec.EllipticCurvePrivateKey

    @property
    def pem(self) -> str:
        return self.certificate.public_bytes(serialization.Encoding.PEM).decode()

    @property
    def der(self) -> bytes:
        return self.certificate.public_bytes(serialization.Encoding.DER)


def _builder(
    common_name: str, issuer: x509.Name, key: ec.EllipticCurvePrivateKey, validity: datetime.timedelta = _VALIDITY
) -> x509.CertificateBuilder:
    now = datetime.datetime.now(datetime.UTC)
    return (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)]))
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(min(now, now + validity) - _CLOCK_SKEW)
        .not_valid_after(now + validity)
    )


def _key_usage(*, digital_signature: bool = False, key_cert_sign: bool = False) -> x509.KeyUsage:
    return x509.KeyUsage(
        digital_signature=digital_signature,
        content_commitment=False,
        key_encipherment=False,
        data_encipherment=False,
        key_agreement=False,
        key_cert_sign=key_cert_sign,
        crl_sign=False,
        encipher_only=False,
        decipher_only=False,
    )


def _with_ca_extensions(builder: x509.CertificateBuilder) -> x509.CertificateBuilder:
    return builder.add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).add_extension(
        _key_usage(key_cert_sign=True), critical=True
    )


def generate_root(common_name: str = "mitto test root", validity: datetime.timedelta = _VALIDITY) -> Issued:
    """Generate a self-signed root that nothing trusts until it is added. A negative `validity` makes it expired."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
    return Issued(_with_ca_extensions(_builder(common_name, name, key, validity)).sign(key, hashes.SHA256()), key)


def generate_intermediate(root: Issued) -> Issued:
    """Generate an intermediate under `root`."""
    key = ec.generate_private_key(ec.SECP256R1())
    builder = _with_ca_extensions(_builder("mitto test intermediate", root.certificate.subject, key))
    return Issued(builder.sign(root.key, hashes.SHA256()), key)


def generate_leaf(issuer: Issued, host: str, aia_url: str | None = None) -> Issued:
    """Generate a certificate for `host` under `issuer`, whose AIA extension names `aia_url` if there is one."""
    key = ec.generate_private_key(ec.SECP256R1())
    builder = (
        _builder(host, issuer.certificate.subject, key)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(host)]), critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(_key_usage(digital_signature=True), critical=True)
    )
    if aia_url is not None:
        builder = builder.add_extension(
            x509.AuthorityInformationAccess(
                [
                    x509.AccessDescription(
                        AuthorityInformationAccessOID.CA_ISSUERS, x509.UniformResourceIdentifier(aia_url)
                    )
                ]
            ),
            critical=False,
        )
    return Issued(builder.sign(issuer.key, hashes.SHA256()), key)


class _AiaHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        server = cast("AiaServer", self.server)
        server.record(self.path, self.headers.items())
        if server.hang:
            server.released.wait()
        body = server.body
        self.send_response(200 if body is not None else 404)
        self.send_header("Content-Length", str(len(body or b"")))
        self.end_headers()
        self.wfile.write(body or b"")

    def log_message(self, format: str, *args: Any) -> None:
        pass


class AiaServer(QuietServer):
    """A local HTTP server that holds one issuer certificate, and records what asks for it.

    Attributes:
        body: What every path answers with, or None for a 404.
        hang: Whether to hold each request until `released` is set.
        requested: Set once a request has arrived.
        released: Lets a held request go on.
        requests: The path and the header lines of each request.
    """

    def __init__(self, body: bytes | None = None, *, hang: bool = False) -> None:
        super().__init__(("127.0.0.1", 0), _AiaHandler)
        self.body = body
        self.hang = hang
        self.requested = threading.Event()
        self.released = threading.Event()
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}{ISSUER_PATH}"

    def record(self, path: str, headers: list[tuple[str, str]]) -> None:
        self.requests.append((path, headers))
        self.requested.set()


class _TlsHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast("TlsServer", self.server)
        self.send_response(server.status)
        for name, value in server.response_headers:
            self.send_header(name, value)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        pass


class TlsServer(QuietServer):
    """A local HTTPS server that presents `leaf` and whatever follows it.

    Attributes:
        status: The status of each response.
        response_headers: The (name, value) pairs that each response holds.
    """

    def __init__(self, directory: Path, leaf: Issued, *chain: Issued) -> None:
        super().__init__(("127.0.0.1", 0), _TlsHandler)
        self.status = 200
        self.response_headers: list[tuple[str, str]] = []
        key = leaf.key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
        ).decode()
        chain_file = directory / f"chain-{self.server_port}.pem"
        chain_file.write_text("".join(item.pem for item in (leaf, *chain)) + key)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(chain_file)
        self.socket = context.wrap_socket(self.socket, server_side=True)


@contextmanager
def serving[ServerT: http.server.HTTPServer](server: ServerT) -> Generator[ServerT]:
    """Run `server` on its own thread, and stop it afterwards."""
    # A short interval, because shutdown() takes this long to be noticed.
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
