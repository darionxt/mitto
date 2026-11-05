"""The normal form of a recording, which is also the Baseline file format.

Two recordings of the same client have the same normal form, although Chrome makes every connection look different.
See docs/capture.md for what stays, what goes, and why.
"""

import json
from collections.abc import Callable, Iterable

from tools.capture.client_hello import ClientHelloRecords, Extension
from tools.capture.http1 import Http1Request
from tools.capture.http2 import (
    ACK,
    CONTINUATION,
    DATA,
    HEADERS,
    PING,
    PRIORITY,
    SETTINGS,
    WINDOW_UPDATE,
    Frame,
    Http2Request,
    parse_priority,
    parse_settings,
    parse_window_update,
)
from tools.capture.names import (
    COMPRESSION_METHODS,
    EC_POINT_FORMATS,
    ECH_CLIENT_HELLO_TYPES,
    GREASE,
    PSK_KEY_EXCHANGE_MODES,
    certificate_compression_name,
    cipher_suite_name,
    ech_aead_name,
    ech_kdf_name,
    extension_name,
    frame_flag_names,
    frame_type_name,
    group_name,
    http2_setting_name,
    one_byte_name,
    signature_algorithm_name,
    version_name,
)
from tools.capture.page import RequestKind, kind_of
from tools.capture.recording import HTTP1, HTTP2, ConnectionRecord, Json, Recording, header_lines
from tools.capture.wire import ParseError, Reader

FORMAT_VERSION = 1

HOST_PLACEHOLDER = "<host>"
AUTHORITY_PLACEHOLDER = "<authority>"
ORIGIN_PLACEHOLDER = "<origin>"


class RecordingError(Exception):
    """A recording has no single normal form."""


def normal_form(recording: Recording) -> dict[str, Json]:
    """Return the normal form of a recording, as JSON data.

    It holds what the connections that carried a Request Kind have in common and, for each Request Kind recorded,
    that kind's first request.

    Raises:
        RecordingError: No connection carried a Request Kind, or those that did do not look the same.
    """
    carriers = [
        connection
        for connection in recording.connections
        if any(kind_of(request.target) is not None for request in connection.requests)
    ]
    if not carriers:
        raise RecordingError("no connection carried a request of any Request Kind")
    first, *others = carriers
    connection_form = _normal_connection(first)
    for other in others:
        if _normal_connection(other) != connection_form:
            raise RecordingError(f"connections {first.id} and {other.id} carried requests but look different")
    requests: dict[str, Json] = {}
    for kind in RequestKind:
        found = _first_request(carriers, kind)
        if found is not None:
            requests[kind.value] = _normal_request(*found, recording)
    return {"format": FORMAT_VERSION, **connection_form, "requests": requests}


def dumps_normal_form(form: dict[str, Json]) -> str:
    """Write a normal form as the text of a Baseline file."""
    return json.dumps(form, indent=2) + "\n"


def _first_request(
    connections: Iterable[ConnectionRecord], kind: RequestKind
) -> tuple[ConnectionRecord, Http1Request | Http2Request] | None:
    for connection in connections:
        for request in connection.requests:
            if kind_of(request.target) is kind:
                return connection, request
    return None


def _normal_connection(connection: ConnectionRecord) -> dict[str, Json]:
    form: dict[str, Json] = {
        "protocol": HTTP2 if connection.alpn == HTTP2 else HTTP1,
        "tls": normal_client_hello(connection.client_hello),
    }
    if connection.alpn == HTTP2:
        form["http2"] = {"preface": _normal_preface(connection.frames)}
    return form


def normal_client_hello(records: ClientHelloRecords) -> dict[str, Json]:
    """Return the normal form of a ClientHello. Equal for any two that differ only in what Chrome randomizes."""
    hello = records.hello
    extensions = sorted(hello.extensions, key=_extension_sort_key)
    return {
        "record_version": version_name(records.record_version),
        "legacy_version": version_name(hello.legacy_version),
        "session_id_length": len(hello.session_id),
        "cipher_suites": [cipher_suite_name(suite) for suite in hello.cipher_suites],
        "compression_methods": [one_byte_name(COMPRESSION_METHODS, method) for method in hello.compression_methods],
        "extensions": [_normal_extension(extension) for extension in extensions],
    }


def _extension_sort_key(extension: Extension) -> tuple[bool, int, int]:
    """Order extensions independently of how the client ordered them: GREASE first, then by number."""
    if extension_name(extension.type) == GREASE:
        return (False, 0, len(extension.data))
    return (True, extension.type, 0)


def _normal_extension(extension: Extension) -> dict[str, Json]:
    name = extension_name(extension.type)
    if name == GREASE:
        return {"type": name, "length": len(extension.data)}
    if name in _PRESENCE_ONLY:
        return {"type": name}
    decoder = _EXTENSION_DECODERS.get(name)
    if decoder is None:
        return {"type": name, "data": extension.data.hex()} if extension.data else {"type": name}
    reader = Reader(extension.data)
    try:
        fields = decoder(reader)
        reader.finish()
    except ParseError:
        return {"type": name, "malformed": extension.data.hex()}
    return {"type": name, **fields}


def _server_name(reader: Reader) -> dict[str, Json]:
    names = reader.vector(2)
    entries: list[Json] = []
    while not names.empty:
        name_type = names.uint(1)
        names.opaque(2)
        entries.append(HOST_PLACEHOLDER if name_type == 0 else f"0x{name_type:02x}")
    return {"names": entries}


def _supported_groups(reader: Reader) -> dict[str, Json]:
    return {"groups": [group_name(group) for group in reader.vector(2).uints(2)]}


def _ec_point_formats(reader: Reader) -> dict[str, Json]:
    return {"formats": [one_byte_name(EC_POINT_FORMATS, value) for value in reader.vector(1).uints(1)]}


def _signature_algorithms(reader: Reader) -> dict[str, Json]:
    return {"algorithms": [signature_algorithm_name(value) for value in reader.vector(2).uints(2)]}


def _protocol_names(reader: Reader) -> dict[str, Json]:
    names = reader.vector(2)
    protocols: list[Json] = []
    while not names.empty:
        protocols.append(names.opaque(1).decode("latin-1"))
    return {"protocols": protocols}


def _compress_certificate(reader: Reader) -> dict[str, Json]:
    return {"algorithms": [certificate_compression_name(value) for value in reader.vector(1).uints(2)]}


def _supported_versions(reader: Reader) -> dict[str, Json]:
    return {"versions": [version_name(value) for value in reader.vector(1).uints(2)]}


def _psk_key_exchange_modes(reader: Reader) -> dict[str, Json]:
    return {"modes": [one_byte_name(PSK_KEY_EXCHANGE_MODES, value) for value in reader.vector(1).uints(1)]}


def _key_share(reader: Reader) -> dict[str, Json]:
    entries = reader.vector(2)
    shares: list[Json] = []
    while not entries.empty:
        group = entries.uint(2)
        shares.append({"group": group_name(group), "key_length": len(entries.opaque(2))})
    return {"shares": shares}


def _trust_anchors(reader: Reader) -> dict[str, Json]:
    entries = reader.vector(2)
    anchors: list[Json] = []
    while not entries.empty:
        anchors.append(entries.opaque(1).hex())
    return {"anchors": anchors}


def _encrypted_client_hello(reader: Reader) -> dict[str, Json]:
    client_hello_type = reader.uint(1)
    fields: dict[str, Json] = {"client_hello": one_byte_name(ECH_CLIENT_HELLO_TYPES, client_hello_type)}
    if client_hello_type == 0:
        fields["kdf"] = ech_kdf_name(reader.uint(2))
        fields["aead"] = ech_aead_name(reader.uint(2))
        # The configuration ID, the encapsulated key and the payload are random, or sized at random.
        reader.rest()
    return fields


_PRESENCE_ONLY = frozenset({"cookie", "early_data", "padding", "pre_shared_key", "session_ticket"})
"""Extensions whose content changes from one connection to the next, or follows from the rest of the ClientHello."""

_EXTENSION_DECODERS: dict[str, Callable[[Reader], dict[str, Json]]] = {
    "server_name": _server_name,
    "supported_groups": _supported_groups,
    "ec_point_formats": _ec_point_formats,
    "signature_algorithms": _signature_algorithms,
    "application_layer_protocol_negotiation": _protocol_names,
    "compress_certificate": _compress_certificate,
    "supported_versions": _supported_versions,
    "psk_key_exchange_modes": _psk_key_exchange_modes,
    "key_share": _key_share,
    "application_settings": _protocol_names,
    "application_settings_old": _protocol_names,
    "trust_anchors": _trust_anchors,
    "encrypted_client_hello": _encrypted_client_hello,
}


def _normal_preface(frames: Iterable[Frame]) -> list[Json]:
    """Return what a client sends before its first request, leaving out replies to what the server sent."""
    preface: list[Json] = []
    for frame in frames:
        if frame.type == HEADERS:
            break
        if frame.type in {SETTINGS, PING} and frame.flags & ACK:
            continue
        preface.append(_normal_frame(frame))
    return preface


def _normal_frame(frame: Frame) -> dict[str, Json]:
    name = frame_type_name(frame.type)
    # GREASE frames have random flags and payloads as well as random types.
    if name == GREASE:
        return {"type": name}
    try:
        return _decoded_frame(name, frame)
    except ParseError:
        # The connection ended on this frame, but what came before it still counts.
        return {"type": name, "stream": frame.stream_id, "malformed": frame.payload.hex()}


def _decoded_frame(name: str, frame: Frame) -> dict[str, Json]:
    if frame.type == SETTINGS:
        settings: list[Json] = [
            GREASE if http2_setting_name(key) == GREASE else f"{http2_setting_name(key)}: {value}"
            for key, value in parse_settings(frame)
        ]
        return {"type": name, "settings": settings}
    if frame.type == WINDOW_UPDATE:
        return {"type": name, "stream": frame.stream_id, "increment": parse_window_update(frame)}
    if frame.type == PRIORITY:
        priority = parse_priority(frame)
        return {
            "type": name,
            "stream": frame.stream_id,
            "exclusive": priority.exclusive,
            "dependency": priority.dependency,
            "weight": priority.weight,
        }
    return {
        "type": name,
        "stream": frame.stream_id,
        "flags": list[Json](frame_flag_names(frame.type, frame.flags)),
        "payload": frame.payload.hex(),
    }


def _normal_request(
    connection: ConnectionRecord, request: Http1Request | Http2Request, recording: Recording
) -> dict[str, Json]:
    headers = header_lines(tuple((name, _placeholders(value, recording)) for name, value in request.headers))
    if isinstance(request, Http1Request):
        return {"request_line": f"{request.method} {request.target} {request.version}", "headers": headers}
    form: dict[str, Json] = {"flags": list[Json](frame_flag_names(HEADERS, request.flags))}
    if request.priority is not None:
        # The dependency is left out: it names whichever stream happened to be open when this one started.
        form["priority"] = {"exclusive": request.priority.exclusive, "weight": request.priority.weight}
    form["headers"] = headers
    later_frames = _later_frames(connection.frames, request.stream_id)
    if later_frames:
        form["later_frames"] = later_frames
    return form


def _later_frames(frames: Iterable[Frame], stream_id: int) -> list[Json]:
    """Return what the client sent on a stream after the header block that opened it."""
    later: list[Json] = []
    headers_seen = False
    for frame in frames:
        if frame.stream_id != stream_id:
            continue
        if headers_seen and frame.type != CONTINUATION:
            later.append(_later_frame(frame))
        headers_seen = headers_seen or frame.type == HEADERS
    return later


def _later_frame(frame: Frame) -> dict[str, Json]:
    form = _normal_frame(frame)
    # Which stream this is, and which it depends on, follows from what happened to be open.
    form.pop("stream", None)
    form.pop("dependency", None)
    # A body is the application's, and how a header block is encoded depends on the requests before it.
    if frame.type in {DATA, HEADERS}:
        form.pop("payload", None)
    return form


def _placeholders(value: str, recording: Recording) -> str:
    """Hide where the server happened to be, which says nothing about the client."""
    return value.replace(f"https://{recording.authority}", ORIGIN_PLACEHOLDER).replace(
        recording.authority, AUTHORITY_PLACEHOLDER
    )
