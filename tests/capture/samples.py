"""Inputs for the capture kit's tests: ClientHellos built to order, and recordings of a real Chrome.

The files in data/ hold what Chrome 156 on macOS sent to the capture server from a fresh user data directory:
chrome_*.json is the input to replay, and chrome_*.normal.json the normal form that recording had at the time.
"""

import json
from dataclasses import dataclass
from pathlib import Path

from aioquic.quic.crypto import CryptoPair
from hpack import Encoder

from tools.capture.client_hello import ClientHelloAssembler
from tools.capture.http1 import Http1Connection
from tools.capture.http2 import END_HEADERS, END_STREAM, HEADERS, PREFACE, Http2Connection, serialize_frame
from tools.capture.http3 import Http3Connection, serialize_varint
from tools.capture.page import respond
from tools.capture.quic import CRYPTO, QUIC_V1, InitialAssembler
from tools.capture.recording import ConnectionRecord, QuicConnectionRecord, Recording

DATA = Path(__file__).parent / "data"
HOST = "capture.localhost"
AUTHORITY = "capture.localhost:8444"

GREASE_A = 0x0A0A
GREASE_B = 0xFAFA

SERVER_NAME = 0
STATUS_REQUEST = 5
SUPPORTED_GROUPS = 10
SIGNATURE_ALGORITHMS = 13
ALPN = 16
SESSION_TICKET = 35
PRE_SHARED_KEY = 41
SUPPORTED_VERSIONS = 43
KEY_SHARE = 51
ENCRYPTED_CLIENT_HELLO = 0xFE0D


def vector(data: bytes, length_size: int = 2) -> bytes:
    return len(data).to_bytes(length_size) + data


def extension(extension_type: int, data: bytes = b"") -> bytes:
    return extension_type.to_bytes(2) + vector(data)


def u16s(*values: int) -> bytes:
    return b"".join(value.to_bytes(2) for value in values)


def server_name(name: bytes = HOST.encode()) -> bytes:
    return extension(SERVER_NAME, vector(b"\x00" + vector(name)))


def supported_groups(*groups: int) -> bytes:
    return extension(SUPPORTED_GROUPS, vector(u16s(*groups)))


def alpn(*protocols: bytes) -> bytes:
    return extension(ALPN, vector(b"".join(vector(protocol, 1) for protocol in protocols)))


def key_share(*shares: tuple[int, bytes]) -> bytes:
    return extension(KEY_SHARE, vector(b"".join(group.to_bytes(2) + vector(key) for group, key in shares)))


def encrypted_client_hello(config_id: int = 7, enc: bytes = bytes(32), payload: bytes = bytes(144)) -> bytes:
    fields = b"\x00" + u16s(1, 1) + bytes((config_id,)) + vector(enc) + vector(payload)
    return extension(ENCRYPTED_CLIENT_HELLO, fields)


def client_hello_message(
    extensions: list[bytes] | None = None,
    *,
    random: bytes = bytes(32),
    session_id: bytes = bytes(32),
    cipher_suites: tuple[int, ...] = (GREASE_A, 0x1301, 0x1302),
) -> bytes:
    """Return a ClientHello handshake message. The defaults are all valid."""
    blocks = [] if extensions is None else extensions
    body = (
        (0x0303).to_bytes(2)
        + random
        + vector(session_id, 1)
        + vector(u16s(*cipher_suites))
        + vector(b"\x00", 1)
        + vector(b"".join(blocks))
    )
    return b"\x01" + vector(body, 3)


def records(message: bytes, *, record_size: int = 1 << 14, version: int = 0x0301) -> bytes:
    """Wrap a handshake message in TLS records of at most record_size bytes each."""
    output = bytearray()
    for start in range(0, len(message), record_size):
        output += b"\x16" + version.to_bytes(2) + vector(message[start : start + record_size])
    return bytes(output)


def client_hello_records(
    extensions: list[bytes] | None = None,
    *,
    random: bytes = bytes(32),
    session_id: bytes = bytes(32),
    cipher_suites: tuple[int, ...] = (GREASE_A, 0x1301, 0x1302),
) -> bytes:
    return records(client_hello_message(extensions, random=random, session_id=session_id, cipher_suites=cipher_suites))


def http2_request_frame(target: str, *, stream_id: int = 1, encoder: Encoder | None = None) -> bytes:
    """Return a GET in one HEADERS frame."""
    block = (encoder or Encoder()).encode(
        [(":method", "GET"), (":authority", AUTHORITY), (":scheme", "https"), (":path", target)]
    )
    return serialize_frame(HEADERS, END_STREAM | END_HEADERS, stream_id, block)


def replay_connection(
    connection_id: int, client_hello: bytes, alpn_protocol: str | None, client_stream: bytes
) -> ConnectionRecord:
    """Return the record a connection leaves when a client sends client_stream over it."""
    found = ClientHelloAssembler().feed(client_hello)
    assert found is not None
    if alpn_protocol == "h2":
        http2 = Http2Connection(respond)
        if client_stream:
            http2.receive(client_stream)
        return ConnectionRecord(
            connection_id,
            found,
            "TLSv1.3",
            None,
            alpn_protocol,
            tuple(http2.frames),
            tuple(http2.requests),
            None,
        )
    http1 = Http1Connection(respond)
    http1.receive(client_stream)
    return ConnectionRecord(connection_id, found, "TLSv1.3", None, alpn_protocol, (), tuple(http1.requests), None)


def http2_stream(*frames: bytes) -> bytes:
    return PREFACE + b"".join(frames)


CONNECTION_ID = bytes.fromhex("8394c8f03e515708")


def crypto_frame(offset: int, data: bytes) -> bytes:
    return serialize_varint(CRYPTO) + serialize_varint(offset) + serialize_varint(len(data)) + data


def initial_packet(
    payload: bytes,
    *,
    destination: bytes = CONNECTION_ID,
    source: bytes = b"",
    token: bytes = b"",
    packet_number: int = 0,
    keys_from: bytes | None = None,
    first_byte: int = 0xC0,
    version: int = QUIC_V1,
) -> bytes:
    """Return an Initial packet as a client protects it, with keys from keys_from if that is not its destination."""
    crypto = CryptoPair()
    crypto.setup_initial(cid=destination if keys_from is None else keys_from, is_client=True, version=QUIC_V1)
    # Header protection samples the packet at a fixed place, so a packet may not be too short.
    payload = payload.ljust(4, b"\x00")
    packet_number_size = (first_byte & 0x03) + 1
    header = (
        bytes((first_byte,))
        + version.to_bytes(4)
        + vector(destination, 1)
        + vector(source, 1)
        + serialize_varint(len(token))
        + token
        + serialize_varint(packet_number_size + len(payload) + 16)
        + packet_number.to_bytes(packet_number_size)
    )
    return crypto.encrypt_packet(header, payload, packet_number)


def initial_datagrams(message: bytes, *, packets: int = 2, destination: bytes = CONNECTION_ID) -> list[bytes]:
    """Return a handshake message split over packets, with its CRYPTO frames in reverse order in each."""
    size = len(message) // packets + 1
    datagrams: list[bytes] = []
    for number, start in enumerate(range(0, len(message), size)):
        piece = message[start : start + size]
        middle = len(piece) // 2
        frames = (
            crypto_frame(start + middle, piece[middle:]) + b"\x01\x00\x00" + crypto_frame(start, piece[:middle])
        ).ljust(1200, b"\x00")
        datagrams.append(initial_packet(frames, destination=destination, packet_number=number))
    return datagrams


def transport_parameter(identifier: int, value: bytes = b"") -> bytes:
    return serialize_varint(identifier) + serialize_varint(len(value)) + value


def varint_parameter(identifier: int, value: int) -> bytes:
    return transport_parameter(identifier, serialize_varint(value))


def replay_quic_connection(
    connection_id: int,
    datagrams: list[bytes],
    streams: list[tuple[int, bytes]],
    *,
    alpn_protocol: str | None = "h3",
    ip_version: int = 6,
) -> QuicConnectionRecord:
    """Return the record a connection leaves when a client sends these datagrams, and then these bytes on each stream.

    Bytes for a unidirectional stream start with its type. Those streams should come first, because what they hold can
    unblock a request.
    """
    assembler = InitialAssembler()
    found = None
    for datagram in datagrams:
        found = assembler.feed(datagram)
    assert found is not None
    http3 = Http3Connection(respond)
    for stream_id, data in streams:
        http3.receive(stream_id, data, stream_id % 4 == 0)
    return QuicConnectionRecord(
        connection_id,
        found,
        ip_version,
        "TLSv1.3",
        None,
        alpn_protocol,
        http3.streams,
        tuple(http3.frames),
        tuple(http3.requests),
        None,
    )


@dataclass(frozen=True, slots=True)
class RealChromeHttp3:
    """A recording of a real Chrome over HTTP/3 and the normal form it had when it was made."""

    recording: Recording
    normal_form: dict[str, object]
    datagrams: tuple[tuple[bytes, ...], ...]
    streams: tuple[tuple[tuple[int, bytes], ...], ...]


def real_chrome_http3() -> RealChromeHttp3:
    """Replay data/chrome_http3.json."""
    source = json.loads((DATA / "chrome_http3.json").read_text(encoding="utf-8"))
    connections: list[QuicConnectionRecord] = []
    all_datagrams: list[tuple[bytes, ...]] = []
    all_streams: list[tuple[tuple[int, bytes], ...]] = []
    for number, connection in enumerate(source["connections"], start=1):
        datagrams = [bytes.fromhex(datagram) for datagram in connection["datagrams"]]
        streams = [
            (
                stream["stream"],
                (serialize_varint(stream["type_id"]) if "type_id" in stream else b"") + bytes.fromhex(stream["data"]),
            )
            for stream in connection["streams"]
        ]
        # Unidirectional streams first, because they hold what the requests' headers refer to.
        streams.sort(key=lambda stream: (stream[0] % 4 == 0, stream[0]))
        connections.append(replay_quic_connection(number, datagrams, streams, ip_version=connection["ip_version"]))
        all_datagrams.append(tuple(datagrams))
        all_streams.append(tuple(streams))
    return RealChromeHttp3(
        Recording(source["host"], source["authority"], tuple(connections)),
        json.loads((DATA / "chrome_http3.normal.json").read_text(encoding="utf-8")),
        tuple(all_datagrams),
        tuple(all_streams),
    )


@dataclass(frozen=True, slots=True)
class RealChrome:
    """A recording of a real Chrome and the normal form it had when it was made."""

    recording: Recording
    normal_form: dict[str, object]
    client_hellos: tuple[bytes, ...]
    client_streams: tuple[bytes, ...]


def real_chrome(stem: str) -> RealChrome:
    """Replay data/chrome_<stem>.json, for stem http1 or http2."""
    source = json.loads((DATA / f"chrome_{stem}.json").read_text(encoding="utf-8"))
    connections: list[ConnectionRecord] = []
    client_hellos: list[bytes] = []
    client_streams: list[bytes] = []
    for number, connection in enumerate(source["connections"], start=1):
        client_hello = bytes.fromhex(connection["client_hello"])
        client_stream = bytes.fromhex(connection["client_stream"])
        connections.append(replay_connection(number, client_hello, connection["alpn"], client_stream))
        client_hellos.append(client_hello)
        client_streams.append(client_stream)
    return RealChrome(
        Recording(source["host"], source["authority"], tuple(connections)),
        json.loads((DATA / f"chrome_{stem}.normal.json").read_text(encoding="utf-8")),
        tuple(client_hellos),
        tuple(client_streams),
    )
