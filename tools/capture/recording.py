"""What the capture server keeps about a connection, and its raw form, which loses nothing that was decoded."""

import contextlib
from dataclasses import dataclass

from tools.capture import http3
from tools.capture.client_hello import ClientHelloRecords
from tools.capture.http1 import Http1Request
from tools.capture.http2 import (
    HEADERS,
    PRIORITY,
    SETTINGS,
    WINDOW_UPDATE,
    Frame,
    Http2Request,
    Priority,
    parse_headers,
    parse_priority,
    parse_settings,
    parse_window_update,
)
from tools.capture.http3 import Http3Frame, Http3Request, Http3Stream
from tools.capture.names import (
    frame_flag_names,
    frame_type_name,
    http2_setting_name,
    http3_frame_type_name,
    http3_setting_name,
    http3_stream_type_name,
    initial_frame_type_name,
    quic_version_name,
)
from tools.capture.quic import CRYPTO, InitialPacket, QuicClientHello
from tools.capture.wire import ParseError

HTTP1 = "http/1.1"
HTTP2 = "h2"
HTTP3 = "h3"
"""The protocols, by the names ALPN knows them by. Each is also a route: the port that offers it, directly."""

PLAIN_HTTP = "plain http"
PROXIED_HTTP1 = "http/1.1 through the proxy"
PROXIED_HTTP2 = "h2 through the proxy"
"""The other routes into the server. A route is what the server keeps one recording of."""

PROXY = "proxy"
"""The port of the server's HTTP proxy. What comes through it counts as PROXIED_HTTP1 or PROXIED_HTTP2."""

type Json = bool | int | str | list[Json] | dict[str, Json] | None


@dataclass(frozen=True, slots=True)
class ConnectionRecord:
    """One connection.

    Attributes:
        id: The connection's number among the server's connections, counted from 1.
        client_hello: The ClientHello that opened it, or nothing on a connection without TLS.
        tls_version: The TLS version the handshake settled on, if it got that far.
        cipher: The cipher suite the handshake settled on, if it got that far.
        alpn: The protocol ALPN settled on, if any. Anything but h2 is served as HTTP/1.1.
        frames: Every HTTP/2 frame received, in order.
        requests: Every request received, in order.
        error: Why the server gave up on the connection, if it did.
        peer: The client's address, without its port, if known.
        connect: The CONNECT request that opened the tunnel this connection came through, if it came through one.
        resumed: Whether the handshake resumed a session from a ticket the server gave out.
    """

    id: int
    client_hello: ClientHelloRecords | None
    tls_version: str | None
    cipher: str | None
    alpn: str | None
    frames: tuple[Frame, ...]
    requests: tuple[Http1Request | Http2Request, ...]
    error: str | None
    peer: str | None = None
    connect: Http1Request | None = None
    resumed: bool = False


@dataclass(frozen=True, slots=True)
class QuicConnectionRecord:
    """One QUIC connection.

    Attributes:
        id: The connection's number among the server's connections, counted from 1.
        client_hello: The ClientHello that opened it, with the Initial packets that carried it.
        ip_version: Whether the client came over IPv4 or IPv6, as 4 or 6.
        tls_version: The TLS version the handshake settled on, if it got that far.
        cipher: The cipher suite the handshake settled on, if it got that far.
        alpn: The protocol ALPN settled on, if any.
        streams: What the client sent on each stream, by stream number.
        frames: Every HTTP/3 frame received on the control and request streams, in the order they arrived.
        requests: Every request received, in order.
        error: Why the connection ended, if it was not for a good reason.
        peer: The client's address, without its port, if known.
        resumed: Whether the handshake resumed a session from a ticket the server gave out.
        early_streams: The streams that carried early data: what arrived on them before the client's Finished.
    """

    id: int
    client_hello: QuicClientHello
    ip_version: int
    tls_version: str | None
    cipher: str | None
    alpn: str | None
    streams: tuple[Http3Stream, ...]
    frames: tuple[Http3Frame, ...]
    requests: tuple[Http3Request, ...]
    error: str | None
    peer: str | None = None
    resumed: bool = False
    early_streams: frozenset[int] = frozenset()


@dataclass(frozen=True, slots=True)
class Recording:
    """Everything one route into the server has received so far.

    Attributes:
        host: The name the page is at, without a port.
        authority: The name and port the page is at.
        connections: The route's connections, in the order they arrived.
        same_site_authority: The name and port that are same-site with the page, if the server has one.
        cross_site_authority: The name and port that are cross-site with the page, if the server has one.
        scheme: The page's scheme.
    """

    host: str
    authority: str
    connections: tuple[ConnectionRecord | QuicConnectionRecord, ...]
    same_site_authority: str | None = None
    cross_site_authority: str | None = None
    scheme: str = "https"


def header_lines(headers: tuple[tuple[str, str], ...]) -> list[Json]:
    """Write (name, value) pairs one line each, the way they read in HTTP/1.1."""
    return [f"{name}: {value}" for name, value in headers]


def raw_form(recording: Recording) -> Json:
    """Return a recording as JSON data: the unchanged ClientHello bytes, every frame, and every request."""
    return {
        "host": recording.host,
        "authority": recording.authority,
        "connections": [_raw_connection(connection) for connection in recording.connections],
    }


def _raw_connection(connection: ConnectionRecord | QuicConnectionRecord) -> Json:
    if isinstance(connection, QuicConnectionRecord):
        return _raw_quic_connection(connection)
    form: dict[str, Json] = {
        "id": connection.id,
        "peer": connection.peer,
        "tls_version": connection.tls_version,
        "cipher": connection.cipher,
        "alpn": connection.alpn,
        "error": connection.error,
        "client_hello": None if connection.client_hello is None else connection.client_hello.raw.hex(),
        "frames": [_raw_frame(frame) for frame in connection.frames],
        "requests": [_raw_request(request) for request in connection.requests],
    }
    if connection.connect is not None:
        form["connect"] = _raw_request(connection.connect)
    if connection.resumed:
        form["resumed"] = True
    return form


def _raw_quic_connection(connection: QuicConnectionRecord) -> Json:
    form: dict[str, Json] = {
        "id": connection.id,
        "peer": connection.peer,
        "ip_version": connection.ip_version,
        "tls_version": connection.tls_version,
        "cipher": connection.cipher,
        "alpn": connection.alpn,
        "error": connection.error,
        "datagrams": [datagram.hex() for datagram in connection.client_hello.datagrams],
        "packets": [_raw_initial_packet(packet) for packet in connection.client_hello.packets],
        "client_hello": connection.client_hello.message.hex(),
        "streams": [_raw_stream(stream) for stream in connection.streams],
        "frames": [_raw_http3_frame(frame) for frame in connection.frames],
        "requests": [
            {"stream": request.stream_id, "headers": header_lines(request.headers)} for request in connection.requests
        ],
    }
    if connection.resumed:
        form["resumed"] = True
    if connection.early_streams:
        form["early_streams"] = list[Json](sorted(connection.early_streams))
    return form


def _raw_initial_packet(packet: InitialPacket) -> Json:
    frames: list[Json] = []
    for frame in packet.frames:
        form: dict[str, Json] = {"type": initial_frame_type_name(frame.type), "size": frame.size}
        if frame.type == CRYPTO:
            form["offset"] = frame.offset
            form["data"] = frame.data.hex()
        frames.append(form)
    return {
        "version": quic_version_name(packet.version),
        "destination_connection_id": packet.destination_connection_id.hex(),
        "source_connection_id": packet.source_connection_id.hex(),
        "token": packet.token.hex(),
        "packet_number": packet.packet_number,
        "packet_number_length": packet.packet_number_length,
        "frames": frames,
    }


def _raw_stream(stream: Http3Stream) -> Json:
    form: dict[str, Json] = {"stream": stream.id}
    if stream.type is not None:
        form["type"] = http3_stream_type_name(stream.type)
        form["type_id"] = stream.type
    form["data"] = stream.data.hex()
    return form


def _raw_http3_frame(frame: Http3Frame) -> Json:
    form: dict[str, Json] = {
        "stream": frame.stream_id,
        "type": http3_frame_type_name(frame.type),
        "type_id": frame.type,
        "payload": frame.payload.hex(),
    }
    if frame.type == http3.SETTINGS:
        # A frame the connection refused is recorded too, so its fields may not decode.
        with contextlib.suppress(ParseError):
            form["settings"] = [f"{http3_setting_name(key)}: {value}" for key, value in http3.parse_settings(frame)]
    return form


def _raw_frame(frame: Frame) -> Json:
    form: dict[str, Json] = {
        "type": frame_type_name(frame.type),
        "type_id": frame.type,
        "flags": list[Json](frame_flag_names(frame.type, frame.flags)),
        "stream": frame.stream_id,
        "payload": frame.payload.hex(),
    }
    # A frame the connection refused is recorded too, so its fields may not decode.
    try:
        if frame.type == SETTINGS:
            form["settings"] = [f"{http2_setting_name(key)}: {value}" for key, value in parse_settings(frame)]
        elif frame.type == WINDOW_UPDATE:
            form["increment"] = parse_window_update(frame)
        elif frame.type == PRIORITY:
            form["priority"] = _raw_priority(parse_priority(frame))
        elif frame.type == HEADERS:
            priority, _ = parse_headers(frame)
            if priority is not None:
                form["priority"] = _raw_priority(priority)
    except ParseError:
        pass
    return form


def _raw_priority(priority: Priority) -> Json:
    return {"exclusive": priority.exclusive, "dependency": priority.dependency, "weight": priority.weight}


def _raw_request(request: Http1Request | Http2Request) -> Json:
    if isinstance(request, Http1Request):
        form: dict[str, Json] = {
            "request_line": f"{request.method} {request.target} {request.version}",
            "headers": header_lines(request.headers),
        }
        if request.body:
            form["body"] = request.body.hex()
        return form
    return {"stream": request.stream_id, "headers": header_lines(request.headers)}
