"""Tests for the ClientHello parser."""

import pytest

from tests.capture import samples
from tools.capture.client_hello import ClientHelloAssembler, Extension, parse_client_hello
from tools.capture.wire import ParseError


def test_reads_every_field() -> None:
    message = samples.client_hello_message(
        [samples.extension(23), samples.extension(0xFF01, b"\x00")],
        random=bytes(range(32)),
        session_id=b"session",
        cipher_suites=(0x1301, 0xC02B),
    )

    hello = parse_client_hello(message)

    assert hello.legacy_version == 0x0303
    assert hello.random == bytes(range(32))
    assert hello.session_id == b"session"
    assert hello.cipher_suites == (0x1301, 0xC02B)
    assert hello.compression_methods == (0,)
    assert hello.extensions == (Extension(23, b""), Extension(0xFF01, b"\x00"))


def test_extensions_block_is_optional() -> None:
    message = samples.client_hello_message()
    # Remove the empty block's two length bytes, and shorten the message's length to match.
    without_block = message[:1] + (len(message) - 6).to_bytes(3) + message[4:-2]

    assert parse_client_hello(without_block).extensions == ()


@pytest.mark.parametrize(
    ("description", "message"),
    [
        ("empty", b""),
        ("another handshake message", b"\x02\x00\x00\x00"),
        ("length past the end", b"\x01\x00\x00\x05\x03\x03"),
        ("cut short", samples.client_hello_message()[:40]),
        ("bytes after the message", samples.client_hello_message() + b"\x00"),
        ("odd cipher suite list", samples.client_hello_message()[:71] + b"\x00\x01\x13" + b"\x01\x00\x00\x00"),
    ],
)
def test_refuses_malformed_messages(description: str, message: bytes) -> None:
    with pytest.raises(ParseError):
        parse_client_hello(message)


def test_refuses_an_extension_that_overruns_the_block() -> None:
    message = samples.client_hello_message([samples.extension(23, b"data")])
    # Claim more extension data than the block holds. The extension's length is the two bytes before its data.
    overrun = bytearray(message)
    overrun[-6:-4] = (200).to_bytes(2)

    with pytest.raises(ParseError):
        parse_client_hello(bytes(overrun))


def test_assembler_waits_for_a_whole_message() -> None:
    raw = samples.client_hello_records([samples.server_name()])
    assembler = ClientHelloAssembler()

    for position in range(len(raw) - 1):
        assert assembler.feed(raw[position : position + 1]) is None
    found = assembler.feed(raw[-1:])

    assert found is not None
    assert found.raw == raw
    assert found.record_version == 0x0301
    assert found.hello.extensions == (Extension(0, samples.vector(b"\x00" + samples.vector(samples.HOST.encode()))),)


def test_assembler_joins_records() -> None:
    message = samples.client_hello_message([samples.server_name()])
    raw = samples.records(message, record_size=3)

    found = ClientHelloAssembler().feed(raw)

    assert found is not None
    assert found.raw == raw
    assert found.hello == parse_client_hello(message)


def test_assembler_leaves_out_what_follows_the_message() -> None:
    raw = samples.client_hello_records()

    found = ClientHelloAssembler().feed(raw + b"\x17\x03\x03\x00\x01\x00")

    assert found is not None
    assert found.raw == raw


@pytest.mark.parametrize(
    ("description", "stream"),
    [
        ("plain HTTP", b"GET / HTTP/1.1\r\n\r\n"),
        ("empty record", b"\x16\x03\x01\x00\x00"),
        ("record over the limit", b"\x16\x03\x01\x40\x01"),
        ("another handshake message", samples.records(b"\x02\x00\x00\x00")),
        ("message over the limit", samples.records(b"\x01\x01\x00\x00")),
    ],
)
def test_assembler_refuses_what_cannot_be_a_client_hello(description: str, stream: bytes) -> None:
    with pytest.raises(ParseError):
        ClientHelloAssembler().feed(stream)


@pytest.mark.parametrize("stem", ["http1", "http2"])
def test_parses_real_chrome(stem: str) -> None:
    for raw in samples.real_chrome(stem).client_hellos:
        found = ClientHelloAssembler().feed(raw)
        assert found is not None
        assert found.raw == raw
        assert 0x1301 in found.hello.cipher_suites
