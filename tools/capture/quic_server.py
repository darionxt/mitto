"""The QUIC side of the capture server: takes datagrams, returns datagrams, and needs no socket of its own.

aioquic is the QUIC and TLS stack. The kit has its own view of the same traffic, which is what gets recorded: Initial
packets go to an InitialAssembler before aioquic sees them, and stream data goes to an Http3Connection after it.
"""

import logging
from collections.abc import Callable

from aioquic.buffer import Buffer  # pyright: ignore[reportPrivateImportUsage]
from aioquic.quic import events
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import NetworkAddress, QuicConnection
from aioquic.quic.packet import QuicPacketType, encode_quic_version_negotiation, pull_quic_header
from cryptography import x509
from cryptography.hazmat.primitives.serialization import load_pem_private_key

from tools.capture.certificate import Certificate
from tools.capture.http3 import Http3Connection, StreamData
from tools.capture.names import cipher_suite_name
from tools.capture.page import Responder
from tools.capture.quic import QUIC_V1, InitialAssembler, QuicClientHello
from tools.capture.recording import HTTP3, QuicConnectionRecord
from tools.capture.wire import ParseError

type Datagram = tuple[bytes, NetworkAddress]

_CONNECTION_ID_LENGTH = 8
_MIN_INITIAL_DATAGRAM_SIZE = 1200
_NO_ERROR = 0
_H3_NO_ERROR = 0x100
_H3_GENERAL_PROTOCOL_ERROR = 0x101
_TLS_VERSION = "TLSv1.3"

_log = logging.getLogger(__name__)


class LiveQuicConnection:
    """A connection in progress: its QUIC stack, the kit's views of its traffic, and how they went so far.

    Attributes:
        id: The connection's number among the server's connections. Whoever keeps it on record sets it.
    """

    def __init__(self, quic: QuicConnection, respond: Responder, ip_version: int) -> None:
        self.id = 0
        self.quic = quic
        self.http = Http3Connection(respond)
        self.ip_version = ip_version
        self.assembler = InitialAssembler()
        self.client_hello: QuicClientHello | None = None
        self.routes = {quic.original_destination_connection_id, quic.host_cid}
        self.tls_version: str | None = None
        self.cipher: str | None = None
        self.alpn: str | None = None
        self.error: str | None = None

    def snapshot(self) -> QuicConnectionRecord:
        assert self.client_hello is not None
        return QuicConnectionRecord(
            self.id,
            self.client_hello,
            self.ip_version,
            self.tls_version,
            self.cipher,
            self.alpn,
            self.http.streams,
            tuple(self.http.frames),
            tuple(self.http.requests),
            self.error,
        )


class QuicDispatcher:
    """Routes datagrams to one QUIC connection per client, each serving HTTP/3.

    Args:
        certificate: What the server presents.
        respond: What answers each connection's requests.
        record: Called with a connection when its ClientHello is complete, which is when it goes on record.
    """

    def __init__(
        self, certificate: Certificate, respond: Responder, record: Callable[[LiveQuicConnection], None]
    ) -> None:
        self._respond = respond
        self._record = record
        self._configuration = QuicConfiguration(
            alpn_protocols=[HTTP3],
            connection_id_length=_CONNECTION_ID_LENGTH,
            is_client=False,
            supported_versions=[QUIC_V1],
        )
        self._configuration.certificate = x509.load_pem_x509_certificate(certificate.certificate_pem)
        self._configuration.private_key = load_pem_private_key(certificate.private_key_pem, password=None)
        self._routes: dict[bytes, LiveQuicConnection] = {}
        self._connections: set[LiveQuicConnection] = set()

    def receive(self, data: bytes, address: NetworkAddress, now: float) -> list[Datagram]:
        """Take a datagram from a client and return what to send back. A datagram that makes no sense is dropped."""
        try:
            header = pull_quic_header(Buffer(data=data), host_cid_length=_CONNECTION_ID_LENGTH)
        except ValueError:
            return []
        if header.packet_type == QuicPacketType.VERSION_NEGOTIATION:
            return []
        if header.version is not None and header.version != QUIC_V1:
            return [(encode_quic_version_negotiation(header.destination_cid, header.source_cid, [QUIC_V1]), address)]
        connection = self._routes.get(header.destination_cid)
        if connection is not None:
            self._assemble(connection, data)
        elif header.packet_type == QuicPacketType.INITIAL and len(data) >= _MIN_INITIAL_DATAGRAM_SIZE:
            connection = self._open(data, header.destination_cid, address)
        if connection is None:
            return []
        connection.quic.receive_datagram(data, address, now)
        return self._drain(connection, now)

    def next_wakeup(self) -> float | None:
        """Return when handle_timers next has something to do, if ever."""
        timers = (connection.quic.get_timer() for connection in self._connections)
        return min((timer for timer in timers if timer is not None), default=None)

    def handle_timers(self, now: float) -> list[Datagram]:
        """Run the timers that are due and return what to send."""
        output: list[Datagram] = []
        for connection in list(self._connections):
            timer = connection.quic.get_timer()
            if timer is not None and timer <= now:
                connection.quic.handle_timer(now)
                output += self._drain(connection, now)
        return output

    def _open(
        self, data: bytes, destination_connection_id: bytes, address: NetworkAddress
    ) -> LiveQuicConnection | None:
        quic = QuicConnection(
            configuration=self._configuration, original_destination_connection_id=destination_connection_id
        )
        connection = LiveQuicConnection(quic, self._respond, 6 if ":" in address[0] else 4)
        try:
            self._feed_assembler(connection, data)
        except ParseError as error:
            # Only a datagram that decrypts opens a connection, so nothing is left waiting for what never comes.
            _log.warning("dropped a datagram that opened no connection: %s", error)
            return None
        for route in connection.routes:
            self._routes[route] = connection
        self._connections.add(connection)
        return connection

    def _assemble(self, connection: LiveQuicConnection, data: bytes) -> None:
        if connection.client_hello is None:
            try:
                self._feed_assembler(connection, data)
            except ParseError as error:
                _log.warning("dropped a datagram while waiting for a ClientHello: %s", error)

    def _feed_assembler(self, connection: LiveQuicConnection, data: bytes) -> None:
        connection.client_hello = connection.assembler.feed(data)
        if connection.client_hello is not None:
            self._record(connection)

    def _drain(self, connection: LiveQuicConnection, now: float) -> list[Datagram]:
        while (event := connection.quic.next_event()) is not None:
            self._handle(connection, event)
        return connection.quic.datagrams_to_send(now)

    def _handle(self, connection: LiveQuicConnection, event: events.QuicEvent) -> None:
        if isinstance(event, events.ProtocolNegotiated):
            connection.alpn = event.alpn_protocol
            if event.alpn_protocol == HTTP3:
                _send(connection, connection.http.start())
        elif isinstance(event, events.HandshakeCompleted):
            connection.tls_version = _TLS_VERSION
            key_schedule = connection.quic.tls.key_schedule
            if key_schedule is not None:
                connection.cipher = cipher_suite_name(key_schedule.cipher_suite)
        elif isinstance(event, events.StreamDataReceived):
            _receive_stream_data(connection, event)
        elif isinstance(event, events.StreamReset):
            connection.http.reset(event.stream_id)
        elif isinstance(event, events.ConnectionIdIssued):
            connection.routes.add(event.connection_id)
            self._routes[event.connection_id] = connection
        elif isinstance(event, events.ConnectionIdRetired):
            connection.routes.discard(event.connection_id)
            self._routes.pop(event.connection_id, None)
        elif isinstance(event, events.ConnectionTerminated):
            self._close(connection, event)

    def _close(self, connection: LiveQuicConnection, event: events.ConnectionTerminated) -> None:
        if connection.error is None and event.error_code not in {_NO_ERROR, _H3_NO_ERROR}:
            connection.error = f"connection closed with error 0x{event.error_code:x}: {event.reason_phrase}"
        if connection.client_hello is None:
            # Nothing recorded this connection, so this is the only trace of it.
            _log.warning("gave up on a connection before its ClientHello was complete: %s", event.reason_phrase)
        for route in connection.routes:
            self._routes.pop(route, None)
        self._connections.discard(connection)


def _receive_stream_data(connection: LiveQuicConnection, event: events.StreamDataReceived) -> None:
    # Once HTTP/3 has failed, what the client still has on its way means nothing.
    if connection.error is not None:
        return
    try:
        _send(connection, connection.http.receive(event.stream_id, event.data, event.end_stream))
    except ParseError as error:
        connection.error = f"{type(error).__name__}: {error}"
        connection.quic.close(error_code=_H3_GENERAL_PROTOCOL_ERROR, reason_phrase=str(error))


def _send(connection: LiveQuicConnection, output: list[StreamData]) -> None:
    for stream_id, data, end_stream in output:
        connection.quic.send_stream_data(stream_id, data, end_stream)
