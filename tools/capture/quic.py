"""Parser for the Initial packets that open a QUIC connection, and for the QUIC data a ClientHello carries.

Anyone can read an Initial packet: its keys come from a connection ID in the packet itself. That lets the capture server
see a ClientHello before its QUIC stack does. A ClientHello can span packets, and clients split it into CRYPTO frames in
any order to keep middleboxes from relying on where it sits, so only the reassembled stream says anything.
"""

from dataclasses import dataclass

from aioquic.buffer import Buffer  # pyright: ignore[reportPrivateImportUsage]
from aioquic.quic.crypto import CryptoPair
from aioquic.quic.packet import QuicHeader, QuicPacketType, QuicProtocolVersion, pull_quic_header

from tools.capture.client_hello import ClientHello, handshake_message_size, parse_client_hello
from tools.capture.wire import ParseError, Reader

QUIC_V1 = int(QuicProtocolVersion.VERSION_1)
"""The only version the capture server speaks."""

PADDING = 0x00
PING = 0x01
ACK = 0x02
ACK_ECN = 0x03
CRYPTO = 0x06
CONNECTION_CLOSE = 0x1C

_MAX_DATAGRAMS = 64
"""How many datagrams may arrive before a ClientHello is complete. Real ones fit in a few."""

_MAX_CRYPTO_SIZE = 1 << 17
"""How far into the handshake stream a CRYPTO frame may reach. A ClientHello is well within this."""


@dataclass(frozen=True, slots=True)
class InitialFrame:
    """A frame of an Initial packet.

    Attributes:
        type: The frame type. A run of PADDING frames counts as one.
        size: How many bytes the frame takes in the packet.
        offset: Where a CRYPTO frame's data goes in the handshake stream. Zero for other frames.
        data: A CRYPTO frame's data. Empty for other frames.
    """

    type: int
    size: int
    offset: int = 0
    data: bytes = b""


@dataclass(frozen=True, slots=True)
class InitialPacket:
    """A decrypted Initial packet.

    Attributes:
        version: The QUIC version.
        destination_connection_id: The connection ID the client addressed the packet to.
        source_connection_id: The connection ID the client wants to be addressed by.
        token: The address validation token.
        packet_number: The packet number, in full.
        packet_number_length: How many bytes the packet number takes on the wire.
        frames: The frames, in order.
    """

    version: int
    destination_connection_id: bytes
    source_connection_id: bytes
    token: bytes
    packet_number: int
    packet_number_length: int
    frames: tuple[InitialFrame, ...]


@dataclass(frozen=True, slots=True)
class QuicClientHello:
    """A ClientHello as it came off a QUIC connection.

    Attributes:
        datagrams: The datagrams that carried it, unchanged and still encrypted.
        packets: The Initial packets in them, decrypted, in the order they arrived.
        message: The reassembled handshake message.
        hello: The parsed message.
    """

    datagrams: tuple[bytes, ...]
    packets: tuple[InitialPacket, ...]
    message: bytes
    hello: ClientHello


def parse_initial_frames(payload: bytes) -> tuple[InitialFrame, ...]:
    """Parse the frames of a decrypted Initial packet.

    Raises:
        ParseError: A frame is malformed, or of a type an Initial packet cannot hold.
    """
    reader = Reader(payload)
    frames: list[InitialFrame] = []
    while not reader.empty:
        before = reader.left
        frame_type = reader.varint()
        offset = 0
        data = b""
        if frame_type == CRYPTO:
            offset = reader.varint()
            data = reader.take(reader.varint())
        elif frame_type in {ACK, ACK_ECN}:
            _skip_ack(reader, frame_type)
        elif frame_type == CONNECTION_CLOSE:
            reader.varint()
            reader.varint()
            reader.take(reader.varint())
        elif frame_type not in {PADDING, PING}:
            raise ParseError(f"frame type 0x{frame_type:02x} in an Initial packet")
        size = before - reader.left
        if frame_type == PADDING and frames and frames[-1].type == PADDING:
            frames[-1] = InitialFrame(PADDING, frames[-1].size + size)
        else:
            frames.append(InitialFrame(frame_type, size, offset, data))
    return tuple(frames)


def _skip_ack(reader: Reader, frame_type: int) -> None:
    reader.varint()  # Largest Acknowledged
    reader.varint()  # ACK Delay
    range_count = reader.varint()
    reader.varint()  # First ACK Range
    for _ in range(range_count):
        reader.varint()  # Gap
        reader.varint()  # ACK Range Length
    if frame_type == ACK_ECN:
        for _ in range(3):
            reader.varint()


class _CryptoStream:
    """The handshake stream, put together from frames that arrive in any order. The first copy of a byte counts."""

    def __init__(self) -> None:
        self._data = bytearray()
        self._present = bytearray()
        self._contiguous = 0

    def add(self, offset: int, data: bytes) -> None:
        end = offset + len(data)
        if end > _MAX_CRYPTO_SIZE:
            raise ParseError(f"CRYPTO data up to offset {end}")
        if end > len(self._data):
            grown = end - len(self._data)
            self._data += bytes(grown)
            self._present += bytes(grown)
        for index, byte in enumerate(data, start=offset):
            if not self._present[index]:
                self._data[index] = byte
                self._present[index] = 1
        while self._contiguous < len(self._present) and self._present[self._contiguous]:
            self._contiguous += 1

    def contiguous(self) -> bytes:
        """Return the bytes from the start of the stream up to the first gap."""
        return bytes(self._data[: self._contiguous])


class InitialAssembler:
    """Collects a client's Initial packets until their CRYPTO frames hold a whole ClientHello.

    The keys come from the destination connection ID of the first Initial packet and stay the same afterwards, even when
    later packets are addressed to another connection ID.
    """

    def __init__(self) -> None:
        self._crypto: CryptoPair | None = None
        self._stream = _CryptoStream()
        self._datagrams: list[bytes] = []
        self._packets: list[InitialPacket] = []
        self._next_packet_number = 0

    def feed(self, datagram: bytes) -> QuicClientHello | None:
        """Add a datagram from the client. Returns the ClientHello once it is complete, and None until then.

        A datagram with no Initial packet is ignored. One that fails leaves the assembler as it was, so a stray packet
        does not cost the connection its ClientHello.

        Raises:
            ParseError: The datagram holds an Initial packet that is malformed or does not decrypt, or the handshake
                stream does not start with a valid ClientHello.
        """
        if len(self._datagrams) >= _MAX_DATAGRAMS:
            raise ParseError(f"no ClientHello in {_MAX_DATAGRAMS} datagrams")
        added = False
        try:
            for header, packet, encrypted_offset in _initial_packets(datagram):
                self._add_packet(header, packet, encrypted_offset)
                added = True
        finally:
            if added:
                self._datagrams.append(datagram)
        return self._client_hello() if added else None

    def _add_packet(self, header: QuicHeader, packet: bytes, encrypted_offset: int) -> None:
        crypto = self._crypto or _client_keys(header.destination_cid)
        try:
            plain_header, payload, packet_number = crypto.decrypt_packet(
                packet, encrypted_offset, self._next_packet_number
            )
        except ValueError as error:
            raise ParseError(f"Initial packet does not decrypt: {error}") from error
        frames = parse_initial_frames(payload)
        self._crypto = crypto
        self._next_packet_number = max(self._next_packet_number, packet_number + 1)
        self._packets.append(
            InitialPacket(
                QUIC_V1,
                header.destination_cid,
                header.source_cid,
                header.token,
                packet_number,
                (plain_header[0] & 0x03) + 1,
                frames,
            )
        )
        for frame in frames:
            if frame.type == CRYPTO:
                self._stream.add(frame.offset, frame.data)

    def _client_hello(self) -> QuicClientHello | None:
        stream = self._stream.contiguous()
        size = handshake_message_size(stream)
        if size is None or len(stream) < size:
            return None
        message = stream[:size]
        return QuicClientHello(tuple(self._datagrams), tuple(self._packets), message, parse_client_hello(message))


def _initial_packets(datagram: bytes) -> list[tuple[QuicHeader, bytes, int]]:
    """Return each Initial packet in a datagram: its header, the packet, and where its encrypted part starts."""
    packets: list[tuple[QuicHeader, bytes, int]] = []
    buffer = Buffer(data=datagram)
    while not buffer.eof():
        start = buffer.tell()
        try:
            header = pull_quic_header(buffer, host_cid_length=0)
            if header.version not in {None, QUIC_V1}:
                raise ValueError(f"QUIC version 0x{header.version:08x}")
        except ValueError as error:
            # What follows the last packet may be padding, which does not parse as a packet.
            if packets:
                break
            raise ParseError(f"bad QUIC header: {error}") from error
        if header.version is None:
            # A packet with a short header takes up the rest of the datagram.
            break
        if header.packet_type == QuicPacketType.INITIAL:
            packets.append((header, datagram[start : start + header.packet_length], buffer.tell() - start))
        buffer.seek(start + header.packet_length)
    return packets


def _client_keys(destination_connection_id: bytes) -> CryptoPair:
    crypto = CryptoPair()
    crypto.setup_initial(cid=destination_connection_id, is_client=False, version=QUIC_V1)
    return crypto


def parse_transport_parameters(data: bytes) -> tuple[tuple[int, bytes], ...]:
    """Return the (identifier, value) pairs of a quic_transport_parameters extension, in order.

    Raises:
        ParseError: The data is malformed.
    """
    reader = Reader(data)
    parameters: list[tuple[int, bytes]] = []
    while not reader.empty:
        identifier = reader.varint()
        parameters.append((identifier, reader.take(reader.varint())))
    return tuple(parameters)
