"""What the capture server keeps about a connection, and its raw form, which loses nothing that was decoded."""

from dataclasses import dataclass

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
from tools.capture.names import frame_flag_names, frame_type_name, http2_setting_name
from tools.capture.wire import ParseError

HTTP1 = "http/1.1"
HTTP2 = "h2"
"""The protocols, by the names ALPN knows them by."""

type Json = bool | int | str | list[Json] | dict[str, Json] | None


@dataclass(frozen=True, slots=True)
class ConnectionRecord:
    """One connection.

    Attributes:
        id: The connection's number among the server's connections, counted from 1.
        client_hello: The ClientHello that opened it.
        tls_version: The TLS version the handshake settled on, if it got that far.
        cipher: The cipher suite the handshake settled on, if it got that far.
        alpn: The protocol ALPN settled on, if any. Anything but h2 is served as HTTP/1.1.
        frames: Every HTTP/2 frame received, in order.
        requests: Every request received, in order.
        error: Why the server gave up on the connection, if it did.
    """

    id: int
    client_hello: ClientHelloRecords
    tls_version: str | None
    cipher: str | None
    alpn: str | None
    frames: tuple[Frame, ...]
    requests: tuple[Http1Request | Http2Request, ...]
    error: str | None


@dataclass(frozen=True, slots=True)
class Recording:
    """Everything one port of the server has received so far.

    Attributes:
        host: The name the server is at, without a port.
        authority: The name and port of this recording's port.
        connections: The port's connections, in the order they arrived.
    """

    host: str
    authority: str
    connections: tuple[ConnectionRecord, ...]


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


def _raw_connection(connection: ConnectionRecord) -> Json:
    return {
        "id": connection.id,
        "tls_version": connection.tls_version,
        "cipher": connection.cipher,
        "alpn": connection.alpn,
        "error": connection.error,
        "client_hello": connection.client_hello.raw.hex(),
        "frames": [_raw_frame(frame) for frame in connection.frames],
        "requests": [_raw_request(request) for request in connection.requests],
    }


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
        return {
            "request_line": f"{request.method} {request.target} {request.version}",
            "headers": header_lines(request.headers),
        }
    return {"stream": request.stream_id, "headers": header_lines(request.headers)}
