"""Tests for reading the Initial packets that open a QUIC connection."""

import pytest
from hypothesis import given
from hypothesis import strategies as st

from tests.capture import samples
from tools.capture.client_hello import parse_client_hello
from tools.capture.http3 import serialize_varint
from tools.capture.quic import (
    ACK,
    ACK_ECN,
    CONNECTION_CLOSE,
    CRYPTO,
    PADDING,
    PING,
    InitialAssembler,
    InitialFrame,
    parse_initial_frames,
    parse_transport_parameters,
)
from tools.capture.wire import ParseError, Reader

# The client Initial packet of the worked example in RFC 9001, section A.2.
RFC_9001_PACKET = bytes.fromhex(
    """
    c000000001088394c8f03e5157080000449e7b9aec34d1b1c98dd7689fb8ec11
    d242b123dc9bd8bab936b47d92ec356c0bab7df5976d27cd449f63300099f399
    1c260ec4c60d17b31f8429157bb35a1282a643a8d2262cad67500cadb8e7378c
    8eb7539ec4d4905fed1bee1fc8aafba17c750e2c7ace01e6005f80fcb7df6212
    30c83711b39343fa028cea7f7fb5ff89eac2308249a02252155e2347b63d58c5
    457afd84d05dfffdb20392844ae812154682e9cf012f9021a6f0be17ddd0c208
    4dce25ff9b06cde535d0f920a2db1bf362c23e596d11a4f5a6cf3948838a3aec
    4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3faf6ad760b7bad1db4ba3
    485e8a94dc250ae3fdb41ed15fb6a8e5eba0fc3dd60bc8e30c5c4287e53805db
    059ae0648db2f64264ed5e39be2e20d82df566da8dd5998ccabdae053060ae6c
    7b4378e846d29f37ed7b4ea9ec5d82e7961b7f25a9323851f681d582363aa5f8
    9937f5a67258bf63ad6f1a0b1d96dbd4faddfcefc5266ba6611722395c906556
    be52afe3f565636ad1b17d508b73d8743eeb524be22b3dcbc2c7468d54119c74
    68449a13d8e3b95811a198f3491de3e7fe942b330407abf82a4ed7c1b311663a
    c69890f4157015853d91e923037c227a33cdd5ec281ca3f79c44546b9d90ca00
    f064c99e3dd97911d39fe9c5d0b23a229a234cb36186c4819e8b9c5927726632
    291d6a418211cc2962e20fe47feb3edf330f2c603a9d48c0fcb5699dbfe58964
    25c5bac4aee82e57a85aaf4e2513e4f05796b07ba2ee47d80506f8d2c25e50fd
    14de71e6c418559302f939b0e1abd576f279c4b2e0feb85c1f28ff18f58891ff
    ef132eef2fa09346aee33c28eb130ff28f5b766953334113211996d20011a198
    e3fc433f9f2541010ae17c1bf202580f6047472fb36857fe843b19f5984009dd
    c324044e847a4f4a0ab34f719595de37252d6235365e9b84392b061085349d73
    203a4a13e96f5432ec0fd4a1ee65accdd5e3904df54c1da510b0ff20dcc0c77f
    cb2c0e0eb605cb0504db87632cf3d8b4dae6e705769d1de354270123cb11450e
    fc60ac47683d7b8d0f811365565fd98c4c8eb936bcab8d069fc33bd801b03ade
    a2e1fbc5aa463d08ca19896d2bf59a071b851e6c239052172f296bfb5e724047
    90a2181014f3b94a4e97d117b438130368cc39dbb2d198065ae3986547926cd2
    162f40a29f0c3c8745c0f50fba3852e566d44575c29d39a03f0cda721984b6f4
    40591f355e12d439ff150aab7613499dbd49adabc8676eef023b15b65bfc5ca0
    6948109f23f350db82123535eb8a7433bdabcb909271a6ecbcb58b936a88cd4e
    8f2e6ff5800175f113253d8fa9ca8885c2f552e657dc603f252e1a8e308f76f0
    be79e2fb8f5d5fbbe2e30ecadd220723c8c0aea8078cdfcb3868263ff8f09400
    54da48781893a7e49ad5aff4af300cd804a6b6279ab3ff3afb64491c85194aab
    760d58a606654f9f4400e8b38591356fbf6425aca26dc85244259ff2b19c41b9
    f96f3ca9ec1dde434da7d2d392b905ddf3d1f9af93d1af5950bd493f5aa731b4
    056df31bd267b6b90a079831aaf579be0a39013137aac6d404f518cfd4684064
    7e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d169a44e55c88d4a9a7f9474241
    e221af44860018ab0856972e194cd934
    """
)
MESSAGE = samples.client_hello_message([samples.server_name(), samples.extension(23, bytes(400))])


def test_reads_the_worked_example_of_the_rfc() -> None:
    found = InitialAssembler().feed(RFC_9001_PACKET)

    assert found is not None
    assert found.datagrams == (RFC_9001_PACKET,)
    packet = found.packets[0]
    assert packet.destination_connection_id == samples.CONNECTION_ID
    assert packet.source_connection_id == b""
    assert packet.token == b""
    assert packet.packet_number == 2
    assert packet.packet_number_length == 4
    assert packet.frames[0].type == CRYPTO
    assert packet.frames[1] == InitialFrame(PADDING, 917)
    assert found.hello.legacy_version == 0x0303
    assert found.hello.random.hex().startswith("ebf8fa56")
    assert found.hello.cipher_suites == (0x1301, 0x1302)
    assert found.message == found.packets[0].frames[0].data


def test_a_client_hello_can_span_packets() -> None:
    assembler = InitialAssembler()
    first, second = samples.initial_datagrams(MESSAGE)

    assert assembler.feed(first) is None
    found = assembler.feed(second)

    assert found is not None
    assert found.datagrams == (first, second)
    assert [packet.packet_number for packet in found.packets] == [0, 1]
    assert found.message == MESSAGE
    assert found.hello == parse_client_hello(MESSAGE)


def test_packets_may_arrive_in_any_order() -> None:
    assembler = InitialAssembler()
    first, second = samples.initial_datagrams(MESSAGE)

    assert assembler.feed(second) is None
    found = assembler.feed(first)

    assert found is not None
    assert found.message == MESSAGE
    assert [packet.packet_number for packet in found.packets] == [1, 0]


def test_the_handshake_stream_tolerates_gaps_and_overlaps() -> None:
    frames = (
        samples.crypto_frame(100, MESSAGE[100:])
        + samples.crypto_frame(50, MESSAGE[50:150])
        + samples.crypto_frame(0, MESSAGE[:40])
    )
    assembler = InitialAssembler()
    assert assembler.feed(samples.initial_packet(frames, packet_number=0)) is None

    found = assembler.feed(samples.initial_packet(samples.crypto_frame(30, MESSAGE[30:60]), packet_number=1))

    assert found is not None
    assert found.message == MESSAGE


def test_the_first_copy_of_a_byte_counts() -> None:
    other = bytearray(MESSAGE)
    other[-1] ^= 0xFF
    frames = samples.crypto_frame(0, MESSAGE[:-1]) + samples.crypto_frame(len(MESSAGE) - 1, bytes(other[-1:]))
    found = InitialAssembler().feed(samples.initial_packet(frames + samples.crypto_frame(0, bytes(other))))

    assert found is not None
    assert found.message == bytes(other)


def test_later_packets_keep_the_keys_of_the_first() -> None:
    assembler = InitialAssembler()
    first, _ = samples.initial_datagrams(MESSAGE)
    assert assembler.feed(first) is None

    later = samples.crypto_frame(0, MESSAGE)
    packet = samples.initial_packet(later, destination=b"chosen", keys_from=samples.CONNECTION_ID, packet_number=1)
    found = assembler.feed(packet)

    assert found is not None
    assert found.packets[1].destination_connection_id == b"chosen"
    assert found.message == MESSAGE


def test_coalesced_packets_are_all_read() -> None:
    first = samples.initial_packet(samples.crypto_frame(0, MESSAGE[:100]), packet_number=0)
    second = samples.initial_packet(samples.crypto_frame(100, MESSAGE[100:]), packet_number=1)

    found = InitialAssembler().feed(first + second)

    assert found is not None
    assert found.message == MESSAGE
    assert len(found.packets) == 2
    assert found.datagrams == (first + second,)


def test_padding_after_the_last_packet_does_not_matter() -> None:
    packet = samples.initial_packet(samples.crypto_frame(0, MESSAGE))

    found = InitialAssembler().feed(packet + bytes(200))

    assert found is not None
    assert found.datagrams == (packet + bytes(200),)


def test_packets_of_other_types_in_a_datagram_are_skipped() -> None:
    initial = samples.initial_packet(samples.crypto_frame(0, MESSAGE))
    handshake = bytes((0xE0,)) + (1).to_bytes(4) + b"\x01a\x00" + (3).to_bytes(1) + b"abc"

    found = InitialAssembler().feed(handshake + initial)

    assert found is not None
    assert found.message == MESSAGE


def test_what_follows_the_last_packet_may_look_like_another_version() -> None:
    packet = samples.initial_packet(samples.crypto_frame(0, MESSAGE))
    junk = b"\xc0" + (0xABCD).to_bytes(4) + bytes(16)

    found = InitialAssembler().feed(packet + junk)

    assert found is not None
    assert found.message == MESSAGE


def test_a_datagram_with_no_initial_packet_is_ignored() -> None:
    assembler = InitialAssembler()
    assert assembler.feed(b"\x40" + bytes(32)) is None

    found = assembler.feed(samples.initial_packet(samples.crypto_frame(0, MESSAGE)))

    assert found is not None
    assert len(found.datagrams) == 1


def test_records_the_header_fields_and_frames() -> None:
    payload = b"\x01" + samples.crypto_frame(0, MESSAGE) + bytes(3) + b"\x01\x00"
    packet = samples.initial_packet(payload, source=b"source", token=b"token", first_byte=0xC1, packet_number=1)

    found = InitialAssembler().feed(packet)

    assert found is not None
    initial = found.packets[0]
    assert initial.source_connection_id == b"source"
    assert initial.token == b"token"
    assert initial.packet_number_length == 2
    assert [frame.type for frame in initial.frames] == [PING, CRYPTO, PADDING, PING, PADDING]
    assert initial.frames[2].size == 3


def test_packet_numbers_are_recovered_in_full() -> None:
    assembler = InitialAssembler()
    assert assembler.feed(samples.initial_packet(samples.crypto_frame(0, MESSAGE[:9]), packet_number=0)) is None
    packet = samples.initial_packet(samples.crypto_frame(9, MESSAGE[9:]), packet_number=300, first_byte=0xC1)

    found = assembler.feed(packet)

    assert found is not None
    assert found.packets[1].packet_number == 300


def test_a_packet_that_does_not_decrypt_changes_nothing() -> None:
    assembler = InitialAssembler()
    bad = samples.initial_packet(samples.crypto_frame(0, MESSAGE), keys_from=b"other keys")

    with pytest.raises(ParseError, match="decrypt"):
        assembler.feed(bad)

    found = assembler.feed(samples.initial_packet(samples.crypto_frame(0, MESSAGE)))
    assert found is not None
    assert len(found.datagrams) == 1


def test_a_stray_packet_after_a_good_one_keeps_the_keys() -> None:
    assembler = InitialAssembler()
    first, second = samples.initial_datagrams(MESSAGE)
    assert assembler.feed(first) is None

    with pytest.raises(ParseError, match="decrypt"):
        assembler.feed(samples.initial_packet(b"\x01", destination=b"stray", packet_number=1))

    found = assembler.feed(second)
    assert found is not None
    assert found.datagrams == (first, second)


def test_a_good_packet_stays_when_another_in_its_datagram_fails() -> None:
    assembler = InitialAssembler()
    good = samples.initial_packet(samples.crypto_frame(0, MESSAGE[:9]))
    bad = samples.initial_packet(b"\x01", keys_from=b"other keys", packet_number=1)
    with pytest.raises(ParseError):
        assembler.feed(good + bad)

    found = assembler.feed(samples.initial_packet(samples.crypto_frame(9, MESSAGE[9:]), packet_number=2))

    assert found is not None
    assert found.datagrams[0] == good + bad


@pytest.mark.parametrize(
    "datagram",
    [
        pytest.param(b"\xc0" + (0xABCD).to_bytes(4), id="truncated header"),
        pytest.param(b"\xc0" + (0xABCD).to_bytes(4) + b"\x00\x00\x00\x00", id="another version"),
        pytest.param(b"\x80" + (1).to_bytes(4) + b"\x00\x00\x00\x00", id="no fixed bit"),
        pytest.param(b"\xc0" + (1).to_bytes(4) + b"\x00\x00\x00\x40", id="truncated length"),
        pytest.param(b"\xc0" + (1).to_bytes(4) + b"\x00\x00\x00\x05abcde", id="too short to sample"),
    ],
)
def test_a_malformed_datagram_fails_as_documented(datagram: bytes) -> None:
    with pytest.raises(ParseError):
        InitialAssembler().feed(datagram)


def test_gives_up_on_a_client_hello_that_does_not_come() -> None:
    assembler = InitialAssembler()
    for number in range(64):
        assert assembler.feed(samples.initial_packet(b"\x01", packet_number=number)) is None

    with pytest.raises(ParseError, match="no ClientHello"):
        assembler.feed(samples.initial_packet(b"\x01", packet_number=64))


def test_refuses_a_handshake_stream_that_runs_on() -> None:
    far = samples.crypto_frame(1 << 20, b"x")
    with pytest.raises(ParseError, match="CRYPTO"):
        InitialAssembler().feed(samples.initial_packet(far))


def test_refuses_a_message_over_the_limit() -> None:
    header = b"\x01" + (1 << 20).to_bytes(3)
    with pytest.raises(ParseError, match="ClientHello of"):
        InitialAssembler().feed(samples.initial_packet(samples.crypto_frame(0, header)))


def test_refuses_a_handshake_stream_that_is_no_client_hello() -> None:
    message = b"\x02" + samples.vector(b"hello", 3)
    with pytest.raises(ParseError, match="ClientHello"):
        InitialAssembler().feed(samples.initial_packet(samples.crypto_frame(0, message)))


def test_parses_the_frames_an_initial_packet_can_hold() -> None:
    ack = bytes((ACK,)) + b"\x05\x01\x01\x00\x00\x00"
    ack_ecn = bytes((ACK_ECN,)) + b"\x05\x01\x00\x00\x07\x08\x09"
    close = bytes((CONNECTION_CLOSE,)) + b"\x01\x00\x03bye"
    payload = b"\x00\x00" + b"\x01" + ack + ack_ecn + close + samples.crypto_frame(1, b"xy")

    frames = parse_initial_frames(payload)

    assert frames == (
        InitialFrame(PADDING, 2),
        InitialFrame(PING, 1),
        InitialFrame(ACK, len(ack)),
        InitialFrame(ACK_ECN, len(ack_ecn)),
        InitialFrame(CONNECTION_CLOSE, len(close)),
        InitialFrame(CRYPTO, 5, 1, b"xy"),
    )


@pytest.mark.parametrize(
    "payload",
    [
        pytest.param(b"\x08", id="a frame an Initial packet cannot hold"),
        pytest.param(b"\x06\x00\x05ab", id="truncated CRYPTO"),
        pytest.param(b"\x02\x00\x00\x05\x00", id="truncated ACK ranges"),
        pytest.param(b"\x1c\x00\x00\x05ab", id="truncated reason"),
        pytest.param(b"\x40", id="truncated frame type"),
    ],
)
def test_malformed_frames_fail_as_documented(payload: bytes) -> None:
    with pytest.raises(ParseError):
        parse_initial_frames(payload)


def test_parses_transport_parameters_in_order() -> None:
    data = (
        samples.varint_parameter(1, 30000)
        + samples.transport_parameter(0x3128, b"ORIG")
        + samples.transport_parameter(0xF)
    )

    assert parse_transport_parameters(data) == ((1, b"\x80\x00\x75\x30"), (0x3128, b"ORIG"), (0xF, b""))


@pytest.mark.parametrize("data", [b"\x01", b"\x01\x04ab", b"\xc0"])
def test_malformed_transport_parameters_fail_as_documented(data: bytes) -> None:
    with pytest.raises(ParseError):
        parse_transport_parameters(data)


@given(st.integers(0, (1 << 62) - 1))
def test_a_varint_reads_back(value: int) -> None:
    reader = Reader(serialize_varint(value))

    assert reader.varint() == value
    assert reader.empty


@pytest.mark.parametrize(
    ("data", "value"),
    [
        (b"\x25", 37),
        (b"\x7b\xbd", 15293),
        (b"\x9d\x7f\x3e\x7d", 494878333),
        (bytes.fromhex("c2197c5eff14e88c"), 151288809941952652),
    ],
)
def test_reads_the_varint_examples_of_the_rfc(data: bytes, value: int) -> None:
    assert Reader(data).varint() == value
