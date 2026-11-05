"""Tests for the normal form: what it hides, what it must not hide, and that it reads a real Chrome."""

import json
from dataclasses import dataclass

import pytest
from hpack import Encoder
from hypothesis import given
from hypothesis import strategies as st

from tests.capture import samples
from tools.capture.client_hello import ClientHelloAssembler
from tools.capture.http2 import (
    END_HEADERS,
    END_STREAM,
    HEADERS,
    PRIORITY,
    PRIORITY_FLAG,
    SETTINGS,
    WINDOW_UPDATE,
    Http2Connection,
    serialize_frame,
)
from tools.capture.normal_form import RecordingError, dumps_normal_form, normal_client_hello, normal_form
from tools.capture.page import respond
from tools.capture.recording import ConnectionRecord, Json, Recording, raw_form
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
    assert fetch["later_frames"] == [{"type": name, "stream": 1, "malformed": "ff"}]
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


def test_dumped_form_is_stable_readable_json() -> None:
    form = normal_form(http1_recording(b"GET / HTTP/1.1\r\nName: \xe9\r\n\r\n"))

    text = dumps_normal_form(form)

    assert text.endswith("}\n")
    assert text.isascii()
    assert '\n  "protocol": "http/1.1",\n' in text
    assert json.loads(text) == form
    assert dumps_normal_form(json.loads(text)) == text
