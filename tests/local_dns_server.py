"""A DNS server on loopback, for tests of how a Session resolves names."""

import socket
import socketserver
import threading
from collections.abc import Sequence
from dataclasses import dataclass
from types import TracebackType
from typing import Self, cast

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

from tools.capture.wire import Reader

TYPE_A = 1
TYPE_HTTPS = 65

_HEADER_SIZE = 12
_CLASS_IN = 1
_TTL_SECONDS = 60
_NAME_AT_THE_QUESTION = bytes.fromhex("c00c")
_RESPONSE_NO_ERROR = bytes.fromhex("8180")

_SVC_PARAM_ALPN = 1
_SVC_PARAM_ECH = 5

# The draft version that ECH shipped with, and the only KEM, KDF and AEAD that the test needs.
_ECH_VERSION = 0xFE0D
DHKEM_X25519_HKDF_SHA256 = 0x0020
ECH_KDF_HKDF_SHA256 = 0x0001
ECH_AEAD_AES_128_GCM = 0x0001


@dataclass(frozen=True, slots=True)
class DnsQuery:
    """One question that the server received."""

    name: str
    type: int


def _opaque(size_bytes: int, data: bytes) -> bytes:
    return len(data).to_bytes(size_bytes) + data


def ech_config_list(config_id: int, public_name: str, public_key: bytes | None = None) -> bytes:
    """Return an ECHConfigList with one configuration, for an X25519 public key or one whose private key nobody keeps."""
    if public_key is None:
        public_key = X25519PrivateKey.generate().public_key().public_bytes_raw()
    contents = b"".join(
        (
            config_id.to_bytes(1),
            DHKEM_X25519_HKDF_SHA256.to_bytes(2),
            _opaque(2, public_key),
            _opaque(2, ECH_KDF_HKDF_SHA256.to_bytes(2) + ECH_AEAD_AES_128_GCM.to_bytes(2)),
            # maximum_name_length, which only decides how much padding the client adds.
            (0).to_bytes(1),
            _opaque(1, public_name.encode()),
            # No extensions.
            _opaque(2, b""),
        )
    )
    return _opaque(2, _ECH_VERSION.to_bytes(2) + _opaque(2, contents))


def https_record(target: str, *, alpn: Sequence[str], ech: bytes | None = None) -> bytes:
    """Return the data of an HTTPS record that says `target` serves these protocols.

    A test's server is not on port 443, so the record's own name is "_PORT._https.HOST". The record has to name the host
    as its target, because //net only uses it for a name that it also has addresses for.
    """
    # Priority 1, which makes it a record of a service.
    data = (1).to_bytes(2) + b"".join(_opaque(1, label.encode()) for label in target.split(".")) + b"\0"
    data += _SVC_PARAM_ALPN.to_bytes(2) + _opaque(2, b"".join(_opaque(1, protocol.encode()) for protocol in alpn))
    if ech is not None:
        data += _SVC_PARAM_ECH.to_bytes(2) + _opaque(2, ech)
    return data


def _answer(record_type: int, data: bytes) -> bytes:
    return b"".join(
        (
            _NAME_AT_THE_QUESTION,
            record_type.to_bytes(2),
            _CLASS_IN.to_bytes(2),
            _TTL_SECONDS.to_bytes(4),
            _opaque(2, data),
        )
    )


class LocalDnsServer(socketserver.UDPServer):
    """Answers every name with 127.0.0.1 and, if it has one, an HTTPS record, and has no records of any other type.

    Use it as a context manager. It records what it was asked in `queries`.
    """

    def __init__(self, https_record: bytes | None = None) -> None:
        super().__init__(("127.0.0.1", 0), _DnsHandler)
        self.https_record = https_record
        self.queries: list[DnsQuery] = []

    def __enter__(self) -> Self:
        # A short interval, because shutdown() takes this long to be noticed.
        threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.shutdown()
        self.server_close()

    @property
    def address(self) -> str:
        """The value for the dns_nameserver setting of a Session."""
        return f"127.0.0.1:{self.server_address[1]}"

    def records_for(self, query: DnsQuery) -> list[bytes]:
        """Return the answer section for `query`."""
        if query.type == TYPE_A:
            return [_answer(TYPE_A, socket.inet_aton("127.0.0.1"))]
        if query.type == TYPE_HTTPS and self.https_record is not None:
            return [_answer(TYPE_HTTPS, self.https_record)]
        return []


class _DnsHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        server = cast(LocalDnsServer, self.server)
        message, sock = cast(tuple[bytes, socket.socket], self.request)
        reader = Reader(message[_HEADER_SIZE:])
        labels: list[str] = []
        while size := reader.uint(1):
            labels.append(reader.take(size).decode())
        query = DnsQuery(".".join(labels), reader.uint(2))
        reader.uint(2)
        question = message[_HEADER_SIZE : len(message) - reader.left]

        server.queries.append(query)
        answers = server.records_for(query)
        header = b"".join(
            (
                message[:2],
                _RESPONSE_NO_ERROR,
                (1).to_bytes(2),
                len(answers).to_bytes(2),
                # No authority or additional records.
                bytes(4),
            )
        )
        sock.sendto(header + question + b"".join(answers), self.client_address)
