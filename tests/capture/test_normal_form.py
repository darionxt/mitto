"""Tests for the normal form: what it hides, what it must not hide, and that it reads a real Chrome."""

import json
from dataclasses import dataclass

import pytest
from hpack import Encoder
from hypothesis import given
from hypothesis import strategies as st
from pylsqpack import Encoder as PylsqpackEncoder

from tests.capture import samples
from tools.capture import MAIN_CASES, http3
from tools.capture.client_hello import ClientHelloAssembler
from tools.capture.http2 import (
    DATA,
    END_HEADERS,
    END_STREAM,
    HEADERS,
    PADDED,
    PRIORITY,
    PRIORITY_FLAG,
    SETTINGS,
    WINDOW_UPDATE,
    Http2Connection,
    serialize_frame,
)
from tools.capture.http3 import serialize_varint
from tools.capture.normal_form import (
    HINTS_FROM_THE_MACHINE,
    MACHINE_PLACEHOLDER,
    RecordingError,
    dumps_normal_form,
    normal_client_hello,
    normal_form,
)
from tools.capture.page import respond
from tools.capture.recording import ConnectionRecord, Json, QuicConnectionRecord, Recording, raw_form
from tools.capture.wire import ParseError


def normalize(raw: bytes) -> dict[str, Json]:
    found = ClientHelloAssembler().feed(raw)
    assert found is not None
    return normal_client_hello(found)


def http1_recording(*heads: bytes, authority: str = samples.AUTHORITY, host: str = samples.HOST) -> Recording:
    hello = samples.client_hello_records([samples.server_name()])
    record = samples.replay_connection(1, hello, "http/1.1", b"".join(heads))
    return Recording(host, authority, (record,))


def extensions_of(form: dict[str, Json]) -> list[Json]:
    extensions = form["extensions"]
    assert isinstance(extensions, list)
    return extensions


def extension_named(form: dict[str, Json], name: str) -> dict[str, Json]:
    for extension in extensions_of(form):
        assert isinstance(extension, dict)
        if extension["type"] == name:
            return extension
    raise AssertionError(f"no extension {name}")


@pytest.mark.parametrize("stem", ["http1", "http2"])
def test_real_chrome_gives_the_recorded_normal_form(stem: str) -> None:
    chrome = samples.real_chrome(stem)

    assert normal_form(chrome.recording) == chrome.normal_form


@pytest.mark.parametrize("stem", ["http1", "http2"])
def test_real_chrome_has_every_request_kind(stem: str) -> None:
    requests = normal_form(samples.real_chrome(stem).recording)["requests"]

    assert isinstance(requests, dict)
    assert list(requests) == ["navigation", "fetch", "script", "image"]


def test_real_chrome_connections_differ_only_in_what_drops_out() -> None:
    chrome = samples.real_chrome("http2")

    assert len(set(chrome.client_hellos)) == len(chrome.client_hellos)
    assert all(normalize(raw) == normalize(chrome.client_hellos[0]) for raw in chrome.client_hellos)


def test_real_chrome_recording_has_a_raw_form() -> None:
    chrome = samples.real_chrome("http2")

    form = raw_form(chrome.recording)

    assert isinstance(form, dict)
    connections = form["connections"]
    assert isinstance(connections, list)
    assert len(connections) == len(chrome.client_hellos)
    assert json.loads(json.dumps(form)) == form


@dataclass(frozen=True, slots=True)
class _RandomParts:
    """Everything Chrome's handshake randomizes, one way or another."""

    random: bytes
    session_id: bytes
    x25519_key: bytes
    grease_cipher: int
    grease_group: int
    grease_extension_1: int
    grease_extension_2: int
    ech_config_id: int
    ech_enc: bytes
    ech_payload: bytes
    ticket: bytes
    binders: bytes
    order: list[int]

    def client_hello(self) -> bytes:
        extensions = [
            samples.extension(samples.SESSION_TICKET, self.ticket),
            samples.extension(samples.PRE_SHARED_KEY, self.binders),
            samples.server_name(),
            samples.supported_groups(self.grease_group, 0x11EC, 0x001D),
            samples.alpn(b"h2", b"http/1.1"),
            samples.key_share((self.grease_group, b"\x00"), (0x001D, self.x25519_key)),
            samples.encrypted_client_hello(self.ech_config_id, self.ech_enc, self.ech_payload),
            samples.extension(self.grease_extension_1),
            samples.extension(self.grease_extension_2, b"\x00"),
        ]
        return samples.client_hello_records(
            [extensions[position] for position in self.order],
            random=self.random,
            session_id=self.session_id,
            cipher_suites=(self.grease_cipher, 0x1301, 0x1302),
        )


_RANDOM_PARTS = st.builds(
    _RandomParts,
    random=st.binary(min_size=32, max_size=32),
    session_id=st.binary(min_size=32, max_size=32),
    x25519_key=st.binary(min_size=32, max_size=32),
    grease_cipher=st.sampled_from([0x0A0A, 0x1A1A, 0x8A8A, 0xFAFA]),
    grease_group=st.sampled_from([0x0A0A, 0x2A2A, 0x9A9A, 0xEAEA]),
    grease_extension_1=st.sampled_from([0x0A0A, 0x3A3A]),
    grease_extension_2=st.sampled_from([0x4A4A, 0x5A5A]),
    ech_config_id=st.integers(0, 255),
    ech_enc=st.binary(min_size=32, max_size=32),
    ech_payload=st.binary(min_size=144, max_size=240),
    ticket=st.binary(max_size=64),
    binders=st.binary(max_size=64),
    order=st.permutations(range(9)),
)


@given(_RANDOM_PARTS, _RANDOM_PARTS)
def test_what_is_random_or_permuted_drops_out(first: _RandomParts, second: _RandomParts) -> None:
    assert normalize(first.client_hello()) == normalize(second.client_hello())


_BASE_EXTENSIONS: list[bytes] = [
    samples.server_name(),
    samples.supported_groups(0x0A0A, 0x001D, 0x0017),
    samples.alpn(b"h2", b"http/1.1"),
    samples.key_share((0x001D, bytes(32))),
    samples.encrypted_client_hello(),
]


def _changed(position: int, new: bytes) -> list[bytes]:
    changed = list(_BASE_EXTENSIONS)
    changed[position] = new
    return changed


@pytest.mark.parametrize(
    ("description", "other"),
    [
        ("cipher suite order", samples.client_hello_records(_BASE_EXTENSIONS, cipher_suites=(0x0A0A, 0x1302, 0x1301))),
        (
            "one more cipher suite",
            samples.client_hello_records(_BASE_EXTENSIONS, cipher_suites=(0x0A0A, 0x1301, 0x1302, 0x1303)),
        ),
        ("no GREASE cipher suite", samples.client_hello_records(_BASE_EXTENSIONS, cipher_suites=(0x1301, 0x1302))),
        ("no session ID", samples.client_hello_records(_BASE_EXTENSIONS, session_id=b"")),
        ("group order", samples.client_hello_records(_changed(1, samples.supported_groups(0x0A0A, 0x0017, 0x001D)))),
        ("ALPN order", samples.client_hello_records(_changed(2, samples.alpn(b"http/1.1", b"h2")))),
        ("key share group", samples.client_hello_records(_changed(3, samples.key_share((0x0017, bytes(32)))))),
        ("key share size", samples.client_hello_records(_changed(3, samples.key_share((0x001D, bytes(33)))))),
        ("missing extension", samples.client_hello_records(_BASE_EXTENSIONS[:-1])),
        ("extra extension", samples.client_hello_records([*_BASE_EXTENSIONS, samples.extension(23)])),
        ("record version", samples.records(samples.client_hello_message(_BASE_EXTENSIONS), version=0x0303)),
    ],
)
def test_what_identifies_a_client_stays(description: str, other: bytes) -> None:
    assert normalize(samples.client_hello_records(_BASE_EXTENSIONS)) != normalize(other)


def test_grease_becomes_a_placeholder_wherever_it_appears() -> None:
    form = normalize(
        samples.client_hello_records(
            [
                samples.extension(0x7A7A),
                samples.supported_groups(0x2A2A, 0x001D),
                samples.key_share((0x2A2A, b"\x00")),
            ],
            cipher_suites=(0xDADA, 0x1301),
        )
    )

    assert form["cipher_suites"] == ["GREASE", "TLS_AES_128_GCM_SHA256"]
    assert extensions_of(form)[0] == {"type": "GREASE", "length": 0}
    assert extension_named(form, "supported_groups")["groups"] == ["GREASE", "x25519"]
    assert extension_named(form, "key_share")["shares"] == [{"group": "GREASE", "key_length": 1}]


def test_ech_keeps_only_its_algorithms() -> None:
    form = normalize(samples.client_hello_records([samples.encrypted_client_hello()]))

    assert extension_named(form, "encrypted_client_hello") == {
        "type": "encrypted_client_hello",
        "client_hello": "outer",
        "kdf": "HKDF-SHA256",
        "aead": "AES-128-GCM",
    }


def test_numbers_without_a_name_stay_as_hexadecimal() -> None:
    form = normalize(samples.client_hello_records([samples.extension(0x1234, b"\xab\xcd")], cipher_suites=(0x9999,)))

    assert form["cipher_suites"] == ["0x9999"]
    assert extensions_of(form) == [{"type": "0x1234", "data": "abcd"}]


@pytest.mark.parametrize(
    ("name", "extension_type"),
    [
        ("server_name", samples.SERVER_NAME),
        ("supported_groups", samples.SUPPORTED_GROUPS),
        ("signature_algorithms", samples.SIGNATURE_ALGORITHMS),
        ("application_layer_protocol_negotiation", samples.ALPN),
        ("supported_versions", samples.SUPPORTED_VERSIONS),
        ("key_share", samples.KEY_SHARE),
        ("encrypted_client_hello", samples.ENCRYPTED_CLIENT_HELLO),
    ],
)
def test_a_malformed_extension_shows_instead_of_failing(name: str, extension_type: int) -> None:
    form = normalize(samples.client_hello_records([samples.extension(extension_type, b"\x00\xff")]))

    assert extension_named(form, name) == {"type": name, "malformed": "00ff"}


def test_headers_keep_names_order_and_values() -> None:
    form = normal_form(http1_recording(b"GET /fetch HTTP/1.1\r\nUser-Agent: x\r\nsec-a: 1\r\nAccept: */*\r\n\r\n"))

    assert form["protocol"] == "http/1.1"
    assert "http2" not in form
    assert form["requests"] == {
        "fetch": {"request_line": "GET /fetch HTTP/1.1", "headers": ["User-Agent: x", "sec-a: 1", "Accept: */*"]}
    }


@pytest.mark.parametrize("name", sorted(HINTS_FROM_THE_MACHINE))
def test_a_hint_from_the_machine_keeps_its_place_and_not_its_value(name: str) -> None:
    head = f"GET /fetch HTTP/1.1\r\nUser-Agent: x\r\n{name.upper()}: 1.5\r\nAccept: */*\r\n\r\n".encode()

    requests = normal_form(http1_recording(head))["requests"]

    assert isinstance(requests, dict)
    assert requests["fetch"] == {
        "request_line": "GET /fetch HTTP/1.1",
        "headers": ["User-Agent: x", f"{name.upper()}: {MACHINE_PLACEHOLDER}", "Accept: */*"],
    }


def test_a_hint_that_says_what_chrome_is_keeps_its_value() -> None:
    head = b'GET /fetch HTTP/1.1\r\nsec-ch-ua-full-version: "157.0.8103.39"\r\nsec-ch-ua-arch: "x86"\r\n\r\n'

    requests = normal_form(http1_recording(head))["requests"]

    assert isinstance(requests, dict)
    assert requests["fetch"] == {
        "request_line": "GET /fetch HTTP/1.1",
        "headers": ['sec-ch-ua-full-version: "157.0.8103.39"', 'sec-ch-ua-arch: "x86"'],
    }


@pytest.mark.parametrize(
    ("authority", "host"),
    [("capture.localhost:8444", "capture.localhost"), ("other.localhost:1234", "other.localhost")],
)
def test_where_the_server_is_drops_out(authority: str, host: str) -> None:
    head = (
        f"GET /fetch HTTP/1.1\r\nHost: {authority}\r\nReferer: https://{authority}/\r\nOrigin: https://{authority}\r\n\r\n"
    ).encode()

    form = normal_form(http1_recording(head, authority=authority, host=host))

    requests = form["requests"]
    assert isinstance(requests, dict)
    assert requests["fetch"] == {
        "request_line": "GET /fetch HTTP/1.1",
        "headers": ["Host: <authority>", "Referer: <origin>/", "Origin: <origin>"],
    }


def _http2_recording(*frames: bytes) -> Recording:
    hello = samples.client_hello_records([samples.server_name()])
    record = samples.replay_connection(1, hello, "h2", samples.http2_stream(*frames))
    return Recording(samples.HOST, samples.AUTHORITY, (record,))


def _settings(*pairs: tuple[int, int]) -> bytes:
    return serialize_frame(SETTINGS, 0, 0, b"".join(key.to_bytes(2) + value.to_bytes(4) for key, value in pairs))


def _preface_of(form: dict[str, Json]) -> Json:
    http2 = form["http2"]
    assert isinstance(http2, dict)
    return http2["preface"]


def test_preface_holds_what_comes_before_the_first_request() -> None:
    form = normal_form(
        _http2_recording(
            _settings((1, 65536), (2, 0)),
            serialize_frame(SETTINGS, 1, 0),
            serialize_frame(WINDOW_UPDATE, 0, 0, (15663105).to_bytes(4)),
            serialize_frame(2, 0, 3, b"\x00\x00\x00\x00\xc8"),
            samples.http2_request_frame("/fetch"),
            _settings((3, 100)),
        )
    )

    assert form["protocol"] == "h2"
    assert _preface_of(form) == [
        {"type": "SETTINGS", "settings": ["HEADER_TABLE_SIZE: 65536", "ENABLE_PUSH: 0"]},
        {"type": "WINDOW_UPDATE", "stream": 0, "increment": 15663105},
        {"type": "PRIORITY", "stream": 3, "exclusive": False, "dependency": 0, "weight": 201},
    ]


@pytest.mark.parametrize(("setting_a", "setting_b"), [(0x0A0A, 0xFAFA), (0x1A2A, 0x3A4A)])
def test_grease_settings_and_frames_drop_out(setting_a: int, setting_b: int) -> None:
    def form_with(setting: int, value: int, frame_type: int, flags: int, payload: bytes) -> Json:
        return _preface_of(
            normal_form(
                _http2_recording(
                    _settings((1, 65536), (setting, value)),
                    serialize_frame(frame_type, flags, 0, payload),
                    samples.http2_request_frame("/fetch"),
                )
            )
        )

    first = form_with(setting_a, 1, 0x0B, 0x00, b"a")
    second = form_with(setting_b, 2, 0xE4, 0xFF, b"bcd")

    assert first == second
    assert first == [{"type": "SETTINGS", "settings": ["HEADER_TABLE_SIZE: 65536", "GREASE"]}, {"type": "GREASE"}]


def test_request_keeps_flags_and_weight_but_not_its_stream_or_dependency() -> None:
    def request_on(stream_id: int, dependency: int) -> Json:
        block = Encoder().encode([(":path", "/image.png"), ("priority", "i")])
        payload = (0x80000000 | dependency).to_bytes(4) + b"\x92" + block
        flags = END_STREAM | END_HEADERS | PRIORITY_FLAG
        form = normal_form(_http2_recording(serialize_frame(HEADERS, flags, stream_id, payload)))
        requests = form["requests"]
        assert isinstance(requests, dict)
        return requests["image"]

    assert request_on(1, 0) == request_on(9, 3)
    assert request_on(1, 0) == {
        "flags": ["END_STREAM", "END_HEADERS", "PRIORITY"],
        "priority": {"exclusive": True, "weight": 147},
        "headers": [":path: /image.png", "priority: i"],
    }


def test_frames_after_a_request_stay_with_it() -> None:
    form = normal_form(
        _http2_recording(
            samples.http2_request_frame("/fetch", stream_id=1),
            serialize_frame(0x2A, 0x7F, 1, b"grease"),
            serialize_frame(WINDOW_UPDATE, 0, 3, (5).to_bytes(4)),
        )
    )

    requests = form["requests"]
    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert fetch["later_frames"] == [{"type": "GREASE"}]


def test_later_frames_do_not_say_which_stream_or_hold_what_the_application_sent() -> None:
    def later_frames_on(stream_id: int, dependency: int) -> Json:
        block = Encoder().encode([(":method", "POST"), (":path", "/fetch")])
        form = normal_form(
            _http2_recording(
                serialize_frame(HEADERS, END_HEADERS, stream_id, block),
                serialize_frame(PRIORITY, 0, stream_id, dependency.to_bytes(4) + b"\xc8"),
                serialize_frame(DATA, 0, stream_id, b"body"),
                serialize_frame(HEADERS, END_HEADERS | END_STREAM, stream_id, Encoder().encode([("name", "value")])),
            )
        )
        requests = form["requests"]
        assert isinstance(requests, dict)
        fetch = requests["fetch"]
        assert isinstance(fetch, dict)
        return fetch["later_frames"]

    assert later_frames_on(1, 0) == later_frames_on(3, 1)
    assert later_frames_on(1, 0) == [
        {"type": "PRIORITY", "exclusive": False, "weight": 201},
        {"type": "DATA", "flags": []},
        {"type": "HEADERS", "flags": ["END_STREAM", "END_HEADERS"]},
    ]


def test_an_http2_body_shows_with_its_request_and_leaves_out_padding() -> None:
    block = Encoder().encode([(":method", "POST"), (":path", "/fetch")])
    form = normal_form(
        _http2_recording(
            serialize_frame(HEADERS, END_HEADERS, 1, block),
            serialize_frame(DATA, 0, 1, b"bo"),
            serialize_frame(DATA, PADDED, 1, b"\x01dy\x00"),
            serialize_frame(DATA, END_STREAM, 1),
        )
    )

    requests = form["requests"]
    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert fetch["body"] == "body"


def test_a_request_with_no_body_has_no_body_key() -> None:
    form = normal_form(_http2_recording(samples.http2_request_frame("/fetch")))

    requests = form["requests"]
    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert "body" not in fetch


@pytest.mark.parametrize(
    ("frame_type", "name"),
    [(WINDOW_UPDATE, "WINDOW_UPDATE"), (PRIORITY, "PRIORITY"), (SETTINGS, "SETTINGS")],
)
def test_a_refused_frame_after_a_request_shows_as_malformed(frame_type: int, name: str) -> None:
    connection = Http2Connection(respond)
    with pytest.raises(ParseError):
        connection.receive(
            samples.http2_stream(
                samples.http2_request_frame("/fetch", stream_id=1),
                serialize_frame(frame_type, 0, 1, b"\xff"),
            )
        )
    hello = ClientHelloAssembler().feed(samples.client_hello_records())
    assert hello is not None
    record = ConnectionRecord(
        1, hello, None, None, "h2", tuple(connection.frames), tuple(connection.requests), "gave up"
    )
    recording = Recording(samples.HOST, samples.AUTHORITY, (record,))

    requests = normal_form(recording)["requests"]

    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert fetch["later_frames"] == [{"type": name, "malformed": "ff"}]
    assert json.dumps(raw_form(recording))


def _connection(connection_id: int, extensions: list[bytes], stream: bytes) -> ConnectionRecord:
    return samples.replay_connection(connection_id, samples.client_hello_records(extensions), "http/1.1", stream)


def test_a_connection_without_a_request_kind_does_not_count() -> None:
    preconnect = _connection(1, [samples.extension(23)], b"")
    other_path = _connection(2, [samples.extension(24)], b"GET /done HTTP/1.1\r\n\r\n")
    carrier = _connection(3, [samples.server_name()], b"GET / HTTP/1.1\r\n\r\n")

    form = normal_form(Recording(samples.HOST, samples.AUTHORITY, (preconnect, other_path, carrier)))

    tls = form["tls"]
    assert isinstance(tls, dict)
    assert extension_named(tls, "server_name") == {"type": "server_name", "names": ["<host>"]}


def test_requests_for_other_paths_stay_out() -> None:
    form = normal_form(http1_recording(b"GET / HTTP/1.1\r\n\r\nGET /done HTTP/1.1\r\n\r\nGET /other HTTP/1.1\r\n\r\n"))

    requests = form["requests"]
    assert isinstance(requests, dict)
    assert list(requests) == ["navigation"]


def test_the_first_request_of_a_kind_counts() -> None:
    form = normal_form(http1_recording(b"GET / HTTP/1.1\r\nA: first\r\n\r\nGET / HTTP/1.1\r\nA: second\r\n\r\n"))

    requests = form["requests"]
    assert isinstance(requests, dict)
    assert requests["navigation"] == {"request_line": "GET / HTTP/1.1", "headers": ["A: first"]}


def test_requests_keep_a_fixed_order() -> None:
    form = normal_form(http1_recording(b"GET /image.png HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n"))

    requests = form["requests"]
    assert isinstance(requests, dict)
    assert list(requests) == ["navigation", "image"]


def test_a_recording_with_no_request_kind_has_no_normal_form() -> None:
    with pytest.raises(RecordingError, match="no connection"):
        normal_form(http1_recording(b"GET /other HTTP/1.1\r\n\r\n"))


def test_connections_that_look_different_have_no_single_normal_form() -> None:
    first = _connection(1, [samples.extension(23)], b"GET / HTTP/1.1\r\n\r\n")
    second = _connection(2, [samples.extension(24)], b"GET /fetch HTTP/1.1\r\n\r\n")

    with pytest.raises(RecordingError, match="connections 1 and 2"):
        normal_form(Recording(samples.HOST, samples.AUTHORITY, (first, second)))


_FETCH, _SCRIPT, _IMAGE = MAIN_CASES[1:]


def test_order_follows_the_streams_that_the_client_opened() -> None:
    recording = _http2_recording(
        samples.http2_request_frame("/script.js", stream_id=1),
        samples.http2_request_frame("/fetch", stream_id=3),
        samples.http2_request_frame("/image.png", stream_id=5),
        samples.http2_request_frame("/fetch", stream_id=7),
    )

    form = normal_form(recording, MAIN_CASES, (_IMAGE, _FETCH, _SCRIPT))

    assert list(form) == ["format", "protocol", "tls", "http2", "requests", "order"]
    assert form["order"] == ["script", "fetch", "image"]
    assert "order" not in normal_form(recording, MAIN_CASES)


def test_order_leaves_out_what_was_not_recorded() -> None:
    form = normal_form(_http2_recording(samples.http2_request_frame("/fetch")), MAIN_CASES, (_IMAGE, _FETCH))

    assert form["order"] == ["fetch"]


def test_http1_has_no_order_because_a_client_spreads_its_requests_over_connections() -> None:
    form = normal_form(
        http1_recording(b"GET /image.png HTTP/1.1\r\n\r\nGET /fetch HTTP/1.1\r\n\r\n"), MAIN_CASES, MAIN_CASES
    )

    assert "order" not in form


def test_an_order_over_more_than_one_connection_is_nobodys() -> None:
    hello = samples.client_hello_records([samples.server_name()])
    first = samples.replay_connection(1, hello, "h2", samples.http2_stream(samples.http2_request_frame("/fetch")))
    second = samples.replay_connection(2, hello, "h2", samples.http2_stream(samples.http2_request_frame("/image.png")))
    recording = Recording(samples.HOST, samples.AUTHORITY, (first, second))

    assert "order" not in normal_form(recording, MAIN_CASES)
    with pytest.raises(RecordingError, match="more than one connection"):
        normal_form(recording, MAIN_CASES, (_FETCH, _IMAGE))


def test_dumped_form_is_stable_readable_json() -> None:
    form = normal_form(http1_recording(b"GET / HTTP/1.1\r\nName: \xe9\r\n\r\n"))

    text = dumps_normal_form(form)

    assert text.endswith("}\n")
    assert text.isascii()
    assert '\n  "protocol": "http/1.1",\n' in text
    assert json.loads(text) == form
    assert dumps_normal_form(json.loads(text)) == text


QUIC_TRANSPORT_PARAMETERS = 57
_GREASE_PARAMETER_A = 27
_GREASE_PARAMETER_B = 31 * 1000 + 27
_GREASE_VERSION_A = 0x0A0A0A0A
_GREASE_VERSION_B = 0x3A5A7A5A
_GREASE_FRAME = 0x21 + 0x1F * 3
_GREASE_FRAME_B = 0x21 + 0x1F * 900
_GREASE_SETTING = 0x21 + 0x1F * 7
_GREASE_SETTING_B = 0x21 + 0x1F * 800
_QPACK_HEADERS = [(b":method", b"GET"), (b":authority", samples.AUTHORITY.encode()), (b":path", b"/fetch")]


def transport_parameters_form(*parameters: bytes) -> dict[str, Json]:
    extension = samples.extension(QUIC_TRANSPORT_PARAMETERS, b"".join(parameters))
    return extension_named(normalize(samples.client_hello_records([extension])), "quic_transport_parameters")


def http3_request_frame(stream_id: int = 0, headers: list[tuple[bytes, bytes]] = _QPACK_HEADERS) -> bytes:
    return http3.serialize_frame(http3.HEADERS, PylsqpackEncoder().encode(stream_id, headers)[1])


def control_stream(*frames: bytes) -> bytes:
    return serialize_varint(http3.CONTROL_STREAM) + b"".join(frames)


def priority_update(stream_id: int, priority: bytes) -> bytes:
    return http3.serialize_frame(http3.PRIORITY_UPDATE_REQUEST, serialize_varint(stream_id) + priority)


def quic_connection(
    streams: list[tuple[int, bytes]],
    *,
    connection_id: int = 1,
    destination: bytes = samples.CONNECTION_ID,
    source: bytes = b"",
    extensions: list[bytes] | None = None,
    ip_version: int = 6,
    datagram_size: int = 1200,
) -> QuicConnectionRecord:
    message = samples.client_hello_message([samples.server_name(), *(extensions or [])])
    payload = samples.crypto_frame(0, message)
    # The rest of the packet: its fixed fields, a one-byte packet number, and the AEAD tag.
    padding = datagram_size - len(destination) - len(source) - 27
    packet = samples.initial_packet(payload.ljust(padding, b"\x00"), destination=destination, source=source)
    return samples.replay_quic_connection(connection_id, [packet], streams, ip_version=ip_version)


def quic_recording(*connections: QuicConnectionRecord) -> Recording:
    return Recording(samples.HOST, samples.AUTHORITY, connections)


def test_real_chrome_over_quic_gives_the_recorded_normal_form() -> None:
    chrome = samples.real_chrome_http3()

    assert normal_form(chrome.recording) == chrome.normal_form


def test_real_chrome_over_quic_has_every_request_kind() -> None:
    form = normal_form(samples.real_chrome_http3().recording)

    assert form["protocol"] == "h3"
    requests = form["requests"]
    assert isinstance(requests, dict)
    assert list(requests) == ["navigation", "fetch", "script", "image"]


def test_real_chrome_over_quic_has_a_raw_form_that_loses_nothing() -> None:
    chrome = samples.real_chrome_http3()

    raw = raw_form(chrome.recording)

    assert isinstance(raw, dict)
    connections = raw["connections"]
    assert isinstance(connections, list)
    first = connections[0]
    assert isinstance(first, dict)
    json.dumps(raw)
    assert first["datagrams"] == [datagram.hex() for datagram in chrome.datagrams[0]]
    assert first["ip_version"] == 6
    packets = first["packets"]
    assert isinstance(packets, list)
    packet = packets[0]
    assert isinstance(packet, dict)
    assert packet["version"] == "QUIC v1"
    frames = first["frames"]
    assert isinstance(frames, list)
    frame_types = [frame["type"] for frame in frames if isinstance(frame, dict)]
    assert "SETTINGS" in frame_types
    assert "GREASE" in frame_types
    settings = next(frame for frame in frames if isinstance(frame, dict) and frame["type"] == "SETTINGS")
    assert isinstance(settings, dict)
    assert "QPACK_BLOCKED_STREAMS: 100" in str(settings["settings"])


@given(
    st.lists(st.sampled_from(range(1, 12)), unique=True, min_size=2),
    st.sampled_from([_GREASE_PARAMETER_A, _GREASE_PARAMETER_B]),
    st.binary(max_size=15),
)
def test_transport_parameters_in_any_order_compare_equal(
    identifiers: list[int], grease: int, grease_value: bytes
) -> None:
    plain = [samples.varint_parameter(identifier, 7) for identifier in identifiers]
    first = [samples.transport_parameter(_GREASE_PARAMETER_A, b"a"), *plain]
    second = [*reversed(plain), samples.transport_parameter(grease, grease_value)]

    assert transport_parameters_form(*first) == transport_parameters_form(*second)


def test_transport_parameters_are_decoded_and_sorted() -> None:
    form = transport_parameters_form(
        samples.varint_parameter(0x0E, 2),
        samples.varint_parameter(0x01, 30000),
        samples.transport_parameter(_GREASE_PARAMETER_B, b"random"),
        samples.transport_parameter(0x3128, b"ORIGRVCM"),
        samples.transport_parameter(0x0C),
        samples.transport_parameter(0xABCD, b"\x12\x34"),
        samples.transport_parameter(0xABCE),
    )

    assert form["parameters"] == [
        {"type": "GREASE"},
        {"type": "max_idle_timeout", "value": 30000},
        {"type": "disable_active_migration"},
        {"type": "active_connection_id_limit", "value": 2},
        {"type": "google_connection_options", "options": ["ORIG", "RVCM"]},
        {"type": "0xabcd", "data": "1234"},
        {"type": "0xabce"},
    ]


def test_connection_ids_in_transport_parameters_keep_only_their_length() -> None:
    first = transport_parameters_form(samples.transport_parameter(0x0F, b"abcd"))
    second = transport_parameters_form(samples.transport_parameter(0x0F, b"wxyz"))

    assert first == second
    assert first["parameters"] == [{"type": "initial_source_connection_id", "length": 4}]


def test_a_measured_round_trip_time_drops_out() -> None:
    measured = samples.varint_parameter(0x3127, 12345)

    assert transport_parameters_form(measured, samples.varint_parameter(1, 5)) == transport_parameters_form(
        samples.varint_parameter(1, 5)
    )


@pytest.mark.parametrize("identifier", [0x173E, 0x219BBCD0], ids=["discard", "debugging_sni"])
def test_parameters_that_hold_a_name_or_random_bytes_keep_only_their_presence(identifier: int) -> None:
    form = transport_parameters_form(samples.transport_parameter(identifier, b"anything"))

    parameters = form["parameters"]
    assert isinstance(parameters, list)
    assert parameters[0] == {"type": {0x173E: "discard", 0x219BBCD0: "debugging_sni"}[identifier]}


def test_version_information_keeps_its_versions_with_grease_first() -> None:
    def information(*versions: int) -> bytes:
        return samples.transport_parameter(0x11, b"".join(version.to_bytes(4) for version in versions))

    first = transport_parameters_form(information(1, 1, _GREASE_VERSION_A))
    second = transport_parameters_form(information(1, _GREASE_VERSION_B, 1))

    assert first == second
    assert first["parameters"] == [
        {"type": "version_information", "chosen": "QUIC v1", "others": ["GREASE", "QUIC v1"]}
    ]


def test_an_unknown_version_stays_as_hexadecimal() -> None:
    form = transport_parameters_form(samples.transport_parameter(0x11, (0x12345678).to_bytes(4)))

    assert form["parameters"] == [{"type": "version_information", "chosen": "0x12345678", "others": []}]


@pytest.mark.parametrize(
    ("identifier", "value", "name"),
    [
        (0x01, b"\x40", "max_idle_timeout"),
        (0x01, b"\x01\x02", "max_idle_timeout"),
        (0x11, b"\x00", "version_information"),
        (0x3128, b"abc", "google_connection_options"),
    ],
)
def test_a_malformed_transport_parameter_shows_instead_of_failing(identifier: int, value: bytes, name: str) -> None:
    form = transport_parameters_form(samples.transport_parameter(identifier, value))

    assert form["parameters"] == [{"type": name, "malformed": value.hex()}]


def test_transport_parameters_that_do_not_parse_show_as_malformed() -> None:
    extension = samples.extension(QUIC_TRANSPORT_PARAMETERS, b"\x01\x05ab")

    form = extension_named(normalize(samples.client_hello_records([extension])), "quic_transport_parameters")

    assert form == {"type": "quic_transport_parameters", "malformed": "01056162"}


def test_quic_form_keeps_lengths_but_not_connection_ids() -> None:
    streams = [(0, http3_request_frame())]
    first = normal_form(quic_recording(quic_connection(streams, destination=b"12345678", source=b"abcd")))
    second = normal_form(quic_recording(quic_connection(streams, destination=b"87654321", source=b"wxyz")))

    assert first == second
    assert first["quic"] == {
        "version": "QUIC v1",
        "destination_connection_id_length": 8,
        "source_connection_id_length": 4,
        "token_length": 0,
        "first_packet_number": 0,
        "first_packet_number_length": 1,
        "first_datagram_ip_size": 1248,
    }


def test_quic_form_does_not_depend_on_the_ip_version() -> None:
    streams = [(0, http3_request_frame())]
    # Chrome makes its first datagrams 20 bytes larger on IPv4, whose header is 20 bytes smaller.
    on_ipv6 = quic_connection(streams, ip_version=6, datagram_size=1230)
    on_ipv4 = quic_connection(streams, ip_version=4, datagram_size=1250)

    assert normal_form(quic_recording(on_ipv6)) == normal_form(quic_recording(on_ipv4))


def test_quic_tls_has_no_record_version() -> None:
    tls = normal_form(quic_recording(quic_connection([(0, http3_request_frame())])))["tls"]

    assert isinstance(tls, dict)
    assert "record_version" not in tls
    assert tls["legacy_version"] == "TLS 1.2"


def test_http3_form_holds_settings_and_grease_placeholders() -> None:
    def control(grease_frame: int, grease_setting: int, grease_value: int, payload: bytes) -> bytes:
        settings = (
            serialize_varint(1)
            + serialize_varint(65536)
            + serialize_varint(grease_setting)
            + serialize_varint(grease_value)
        )
        return control_stream(
            http3.serialize_frame(http3.SETTINGS, settings), http3.serialize_frame(grease_frame, payload)
        )

    request = (0, http3_request_frame())
    first = normal_form(
        quic_recording(quic_connection([(2, control(_GREASE_FRAME, _GREASE_SETTING, 1, b"x")), request]))
    )
    second = normal_form(
        quic_recording(quic_connection([(2, control(_GREASE_FRAME_B, _GREASE_SETTING_B, 99, b"yz")), request]))
    )

    assert first == second
    assert first["http3"] == {
        "streams": [{"type": "control", "stream": 2}],
        "control": [
            {"type": "SETTINGS", "settings": ["QPACK_MAX_TABLE_CAPACITY: 65536", "GREASE"]},
            {"type": "GREASE"},
        ],
    }


def test_http3_form_says_which_stream_each_type_got() -> None:
    connection = quic_connection(
        [
            (2, control_stream()),
            (6, serialize_varint(0x21 + 0x1F * 9) + b"abc"),
            (10, serialize_varint(http3.QPACK_ENCODER_STREAM)),
            (14, serialize_varint(0x12345)),
            (0, http3_request_frame()),
        ]
    )

    section = normal_form(quic_recording(connection))["http3"]

    assert isinstance(section, dict)
    assert section["streams"] == [
        {"type": "control", "stream": 2},
        {"type": "GREASE"},
        {"type": "qpack_encoder", "stream": 10},
        {"type": "0x12345", "stream": 14},
    ]


def test_http3_control_leaves_out_what_follows_from_requests_or_leaving() -> None:
    connection = quic_connection(
        [
            (
                2,
                control_stream(
                    http3.serialize_frame(http3.SETTINGS),
                    priority_update(0, b"u=1"),
                    http3.serialize_frame(http3.GOAWAY, b"\x00"),
                    http3.serialize_frame(0x7F00, b"\xab"),
                ),
            ),
            (0, http3_request_frame()),
        ]
    )

    form = normal_form(quic_recording(connection))

    section = form["http3"]
    assert isinstance(section, dict)
    assert section["control"] == [{"type": "SETTINGS", "settings": []}, {"type": "0x7f00", "payload": "ab"}]


def test_http3_requests_keep_their_headers_and_priority_updates() -> None:
    connection = quic_connection(
        [
            (2, control_stream(priority_update(0, b"u=1, i"), priority_update(4, b"u=4"), priority_update(0, b"u=2"))),
            (0, http3_request_frame(headers=[(b":path", b"/fetch"), (b"z", samples.AUTHORITY.encode()), (b"a", b"1")])),
            (4, http3_request_frame(4, [(b":path", b"/image.png")])),
        ]
    )

    requests = normal_form(quic_recording(connection))["requests"]

    assert requests == {
        "fetch": {"headers": [":path: /fetch", "z: <authority>", "a: 1"], "priority_updates": ["u=1, i", "u=2"]},
        "image": {"headers": [":path: /image.png"], "priority_updates": ["u=4"]},
    }


def test_http3_stream_ids_drop_out_of_requests() -> None:
    def recording(stream_id: int) -> Recording:
        return quic_recording(
            quic_connection(
                [(2, control_stream(priority_update(stream_id, b"i"))), (stream_id, http3_request_frame(stream_id))]
            )
        )

    assert normal_form(recording(0)) == normal_form(recording(8))


def test_an_http3_body_shows_with_its_request() -> None:
    stream = http3_request_frame() + http3.serialize_frame(http3.DATA, b"bo") + http3.serialize_frame(http3.DATA, b"dy")

    requests = normal_form(quic_recording(quic_connection([(0, stream), (4, http3_request_frame())])))["requests"]

    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert fetch["body"] == "body"


def test_http3_frames_around_a_request_stay_with_it() -> None:
    stream = (
        http3.serialize_frame(_GREASE_FRAME, b"first")
        + http3_request_frame()
        + http3.serialize_frame(_GREASE_FRAME_B, b"second")
        + http3.serialize_frame(http3.DATA, b"body")
        + http3_request_frame(headers=[(b"trailer", b"1")])
        + http3.serialize_frame(0x7F00, b"\xab")
    )

    requests = normal_form(quic_recording(quic_connection([(0, stream)])))["requests"]

    assert isinstance(requests, dict)
    fetch = requests["fetch"]
    assert isinstance(fetch, dict)
    assert fetch["earlier_frames"] == [{"type": "GREASE"}]
    assert fetch["later_frames"] == [
        {"type": "GREASE"},
        {"type": "DATA"},
        {"type": "HEADERS"},
        {"type": "0x7f00", "payload": "ab"},
    ]


def test_a_malformed_priority_update_does_not_count_for_a_request() -> None:
    connection = quic_connection(
        [
            (2, control_stream(http3.serialize_frame(http3.PRIORITY_UPDATE_REQUEST, b"\x40"))),
            (0, http3_request_frame()),
        ]
    )

    requests = normal_form(quic_recording(connection))["requests"]

    assert requests == {"fetch": {"headers": [":method: GET", ":authority: <authority>", ":path: /fetch"]}}


def test_a_quic_connection_without_a_request_kind_does_not_count() -> None:
    idle = quic_connection([(2, control_stream())], connection_id=1, extensions=[samples.extension(23)])
    used = quic_connection([(0, http3_request_frame())], connection_id=2)

    form = normal_form(quic_recording(idle, used))

    assert form == normal_form(quic_recording(used))


def test_quic_connections_that_look_different_have_no_single_normal_form() -> None:
    first = quic_connection([(0, http3_request_frame())], connection_id=1)
    second = quic_connection([(0, http3_request_frame())], connection_id=2, extensions=[samples.extension(23)])

    with pytest.raises(RecordingError, match="connections 1 and 2"):
        normal_form(quic_recording(first, second))


def test_a_quic_connection_shows_in_the_raw_form_with_what_it_could_not_decode() -> None:
    record = quic_connection(
        [
            (2, control_stream(http3.serialize_frame(http3.SETTINGS, b"\x01\x02"))),
            (6, serialize_varint(http3.QPACK_ENCODER_STREAM)),
        ],
        source=b"abcd",
    )

    raw = raw_form(quic_recording(record))

    assert isinstance(raw, dict)
    connections = raw["connections"]
    assert isinstance(connections, list)
    connection = connections[0]
    assert isinstance(connection, dict)
    assert connection["streams"] == [
        {"stream": 2, "type": "control", "type_id": 0, "data": "04020102"},
        {"stream": 6, "type": "qpack_encoder", "type_id": 2, "data": ""},
    ]
    frames = connection["frames"]
    assert isinstance(frames, list)
    assert frames[0] == {
        "stream": 2,
        "type": "SETTINGS",
        "type_id": 4,
        "payload": "0102",
        "settings": ["QPACK_MAX_TABLE_CAPACITY: 2"],
    }
    packets = connection["packets"]
    assert isinstance(packets, list)
    packet = packets[0]
    assert isinstance(packet, dict)
    assert packet["source_connection_id"] == "61626364"
    packet_frames = packet["frames"]
    assert isinstance(packet_frames, list)
    crypto = packet_frames[0]
    assert isinstance(crypto, dict)
    assert crypto["type"] == "CRYPTO"
    assert crypto["offset"] == 0
