"""The private certificate the capture server presents."""

import base64
import datetime
import hashlib
from dataclasses import dataclass

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

_VALIDITY = datetime.timedelta(days=30)
_CLOCK_SKEW = datetime.timedelta(days=1)


@dataclass(frozen=True, slots=True)
class Certificate:
    """A certificate and its key.

    Attributes:
        certificate_pem: The certificate in PEM form.
        private_key_pem: The private key in unencrypted PEM form. Nothing trusts this certificate, so it is no secret.
        spki_hash: The base64 SHA-256 of the public key, as --ignore-certificate-errors-spki-list wants it.
    """

    certificate_pem: bytes
    private_key_pem: bytes
    spki_hash: str


def generate_certificate(host: str) -> Certificate:
    """Generate a new self-signed certificate for host."""
    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.UTC)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host)])
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - _CLOCK_SKEW)
        .not_valid_after(now + _VALIDITY)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(host)]), critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(key, hashes.SHA256())
    )
    spki = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return Certificate(
        certificate.public_bytes(serialization.Encoding.PEM),
        key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()),
        base64.b64encode(hashlib.sha256(spki).digest()).decode("ascii"),
    )
