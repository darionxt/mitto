"""An ECH key that a test keeps, and what a ClientHello encrypted to it holds: the ClientHelloInner of RFC 9849."""

import hashlib
import hmac
from dataclasses import dataclass
from typing import Self

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from tests.local_dns_server import DHKEM_X25519_HKDF_SHA256, ECH_AEAD_AES_128_GCM, ECH_KDF_HKDF_SHA256, ech_config_list
from tools.capture.client_hello import ClientHello, Extension
from tools.capture.wire import Reader

_ENCRYPTED_CLIENT_HELLO = 65037
_ECH_OUTER_EXTENSIONS = 0xFD00
_OUTER = 0
_AES_128_KEY_SIZE = 16
_NONCE_SIZE = 12
_SECRET_SIZE = 32
# RFC 9180, sections 4 and 5.1, for DHKEM(X25519, HKDF-SHA256), HKDF-SHA256 and AES-128-GCM in the base mode.
_KEM_SUITE = b"KEM" + DHKEM_X25519_HKDF_SHA256.to_bytes(2)
_HPKE_SUITE = (
    b"HPKE" + DHKEM_X25519_HKDF_SHA256.to_bytes(2) + ECH_KDF_HKDF_SHA256.to_bytes(2) + ECH_AEAD_AES_128_GCM.to_bytes(2)
)
_BASE_MODE = b"\0"
# RFC 9849, section 6.1: "tls ech" with its terminating zero, then the ECHConfig.
_INFO_LABEL = b"tls ech\0"


@dataclass(frozen=True, slots=True)
class EchKey:
    """An ECH configuration, for an HTTPS record, with the private key that opens what a client encrypts to it."""

    config_id: int
    private_key: X25519PrivateKey
    config_list: bytes

    @classmethod
    def generate(cls, config_id: int, public_name: str) -> Self:
        private_key = X25519PrivateKey.generate()
        public_key = private_key.public_key().public_bytes_raw()
        return cls(config_id, private_key, ech_config_list(config_id, public_name, public_key))

    def inner_client_hello(self, outer: ClientHello) -> ClientHello:
        """Return the ClientHelloInner that an outer ClientHello carries for this configuration.

        Raises:
            ValueError: The outer ClientHello has no real ECH for this configuration.
            cryptography.exceptions.InvalidTag: The client encrypted it to another key.
        """
        extension = next((item for item in outer.extensions if item.type == _ENCRYPTED_CLIENT_HELLO), None)
        if extension is None:
            raise ValueError("no encrypted_client_hello extension")
        reader = Reader(extension.data)
        if reader.uint(1) != _OUTER or reader.uint(2) != ECH_KDF_HKDF_SHA256 or reader.uint(2) != ECH_AEAD_AES_128_GCM:
            raise ValueError("not an outer ClientHello with this configuration's cipher suite")
        if reader.uint(1) != self.config_id:
            raise ValueError("encrypted to another configuration")
        encapsulated_key = reader.opaque(2)
        payload = reader.opaque(2)
        encoded = self._open(encapsulated_key, payload, _outer_aad(outer, extension.data, len(payload)))
        return _decode_inner(encoded, outer)

    def _open(self, encapsulated_key: bytes, payload: bytes, aad: bytes) -> bytes:
        """HPKE's SetupBaseR and Open, RFC 9180, sections 4.1, 5.1 and 5.2."""
        dh = self.private_key.exchange(X25519PublicKey.from_public_bytes(encapsulated_key))
        kem_context = encapsulated_key + self.private_key.public_key().public_bytes_raw()
        eae_prk = _labeled_extract(_KEM_SUITE, b"", b"eae_prk", dh)
        shared_secret = _labeled_expand(_KEM_SUITE, eae_prk, b"shared_secret", kem_context, _SECRET_SIZE)

        # The ECHConfigList holds one configuration, which follows its 2-byte length.
        info = _INFO_LABEL + self.config_list[2:]
        context = (
            _BASE_MODE
            + _labeled_extract(_HPKE_SUITE, b"", b"psk_id_hash", b"")
            + _labeled_extract(_HPKE_SUITE, b"", b"info_hash", info)
        )
        secret = _labeled_extract(_HPKE_SUITE, shared_secret, b"secret", b"")
        key = _labeled_expand(_HPKE_SUITE, secret, b"key", context, _AES_128_KEY_SIZE)
        nonce = _labeled_expand(_HPKE_SUITE, secret, b"base_nonce", context, _NONCE_SIZE)
        return AESGCM(key).decrypt(nonce, payload, aad)


def _labeled_extract(suite: bytes, salt: bytes, label: bytes, ikm: bytes) -> bytes:
    return hmac.digest(salt or bytes(_SECRET_SIZE), b"HPKE-v1" + suite + label + ikm, hashlib.sha256)


def _labeled_expand(suite: bytes, prk: bytes, label: bytes, info: bytes, size: int) -> bytes:
    labeled_info = size.to_bytes(2) + b"HPKE-v1" + suite + label + info
    output = b""
    block = b""
    counter = 1
    while len(output) < size:
        block = hmac.digest(prk, block + labeled_info + bytes([counter]), hashlib.sha256)
        output += block
        counter += 1
    return output[:size]


def _opaque(size_bytes: int, data: bytes) -> bytes:
    return len(data).to_bytes(size_bytes) + data


def _hello_body(hello: ClientHello, extensions: tuple[Extension, ...]) -> bytes:
    return b"".join(
        (
            hello.legacy_version.to_bytes(2),
            hello.random,
            _opaque(1, hello.session_id),
            _opaque(2, b"".join(suite.to_bytes(2) for suite in hello.cipher_suites)),
            _opaque(1, bytes(hello.compression_methods)),
            _opaque(2, b"".join(item.type.to_bytes(2) + _opaque(2, item.data) for item in extensions)),
        )
    )


def _outer_aad(outer: ClientHello, extension_data: bytes, payload_size: int) -> bytes:
    """ClientHelloOuterAAD, RFC 9849, section 5.2: the ClientHello without its handshake header, payload zeroed."""
    zeroed = extension_data[:-payload_size] + bytes(payload_size)
    extensions = tuple(
        Extension(item.type, zeroed) if item.type == _ENCRYPTED_CLIENT_HELLO else item for item in outer.extensions
    )
    return _hello_body(outer, extensions)


def _decode_inner(encoded: bytes, outer: ClientHello) -> ClientHello:
    """Decode an EncodedClientHelloInner, RFC 9849, section 5.1, with what it takes from the outer ClientHello."""
    reader = Reader(encoded)
    legacy_version = reader.uint(2)
    random = reader.take(32)
    if reader.opaque(1):
        raise ValueError("an EncodedClientHelloInner has a session ID of its own")
    cipher_suites = reader.vector(2).uints(2)
    compression_methods = reader.vector(1).uints(1)
    block = reader.vector(2)
    if any(reader.take(reader.left)):
        raise ValueError("padding that is not zero")
    outer_extensions = {item.type: item for item in outer.extensions}
    extensions: list[Extension] = []
    while not block.empty:
        extension_type = block.uint(2)
        data = block.opaque(2)
        if extension_type == _ECH_OUTER_EXTENSIONS:
            extensions += [outer_extensions[item] for item in Reader(data).vector(1).uints(2)]
        else:
            extensions.append(Extension(extension_type, data))
    return ClientHello(legacy_version, random, outer.session_id, cipher_suites, compression_methods, tuple(extensions))
