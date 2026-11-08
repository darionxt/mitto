"""Tests of tests/network_noise.py, the Python port of Chrome's noise on rtt and downlink, which need no native library.

tests/test_client_hints.py compares what the native core sends with this port, on hosts of each length that the hashes
read in a way of their own, so a fault in either shows there. These pin the port to Chromium's own cases.
"""

import pytest

from tests.network_noise import (
    blink_string_hash,
    browser_downlink,
    browser_rtt,
    rapidhash,
    renderer_downlink,
    renderer_rtt,
    renderer_salt,
)

BROWSER_SALTS = range(1, 22)
RENDERER_SALTS = range(1, 21)


# The hashes Blink's string_hasher_test.cc pins: of nothing, of one NUL and of five Latin-1 characters.
@pytest.mark.parametrize(
    ("data", "expected"),
    [
        (b"", 0x5A6EF77074EBC84B),
        (b"\0", 0x48DFCE108249B3F8),
        (bytes([0x41, 0x95, 0xFF, 0x50, 0x01]), 0xE9422771E0A5DDE6),
    ],
)
def test_rapidhash_gives_what_blinks_tests_pin(data: bytes, expected: int) -> None:
    assert rapidhash(data) == expected
    assert blink_string_hash(data) == expected & 0xFFFFFF


def test_blinks_hash_of_a_string_is_never_0() -> None:
    assert all(blink_string_hash(bytes([byte])) != 0 for byte in range(256))


# The cases of content/browser/client_hints/client_hints_unittest.cc, for each salt that the browser process can draw,
# and for each that a renderer process can.


@pytest.mark.parametrize("host", ["", "foo.com"])
@pytest.mark.parametrize("round_trip_ms", [1023, 6787, 12, 1193, 2789, 6023])
def test_rtt_is_a_multiple_of_50_of_at_most_3000(host: str, round_trip_ms: int) -> None:
    for rtt in _rtts(host, round_trip_ms):
        assert rtt % 50 == 0
        assert rtt <= 3000


@pytest.mark.parametrize("host", ["", "foo.com"])
@pytest.mark.parametrize("throughput_kbps", [102, 12, 2102, 12102, 100102])
def test_downlink_is_a_multiple_of_50_kbps_of_at_most_10_mbps(host: str, throughput_kbps: int) -> None:
    for downlink in _downlinks(host, throughput_kbps):
        assert round(downlink * 1000) % 50 == 0
        assert downlink <= 10


@pytest.mark.parametrize(("round_trip_ms", "within"), [(98, 100), (1023, 200), (1193, 200), (2750, 400)])
def test_rtt_stays_within_about_10_percent(round_trip_ms: int, within: int) -> None:
    assert all(abs(rtt - round_trip_ms) <= within for rtt in _rtts("", round_trip_ms))


@pytest.mark.parametrize(("throughput_kbps", "within"), [(98, 0.1), (1023, 0.2), (1193, 0.2), (7523, 0.9), (9999, 1.2)])
def test_downlink_stays_within_about_10_percent(throughput_kbps: int, within: float) -> None:
    assert all(abs(downlink - throughput_kbps / 1000) <= within for downlink in _downlinks("", throughput_kbps))


def test_rtt_and_downlink_change_with_the_host_and_stay_for_one_host() -> None:
    hosts = [str(number) for number in range(20)]
    for salt in BROWSER_SALTS:
        rtt = browser_rtt("example.com", 1023, salt)
        downlink = browser_downlink("example.com", 1023, salt)
        assert any(browser_rtt(host, 1023, salt) != rtt for host in hosts)
        assert any(browser_downlink(host, 1023, salt) != downlink for host in hosts)
        assert browser_rtt("example.com", 1023, salt) == rtt
    for salt in RENDERER_SALTS:
        rtt = renderer_rtt("example.com", 1023, salt)
        assert any(renderer_rtt(host, 1023, salt) != rtt for host in hosts)


def test_each_salt_gives_a_host_another_multiplier() -> None:
    # 0.9 to 1.1 of 1000 ms, in steps of 10 ms, rounded to 50.
    assert {browser_rtt("foo.com", 1000, salt) for salt in BROWSER_SALTS} == {"900", "950", "1000", "1050", "1100"}
    assert {renderer_rtt("foo.com", 1000, salt) for salt in RENDERER_SALTS} <= {"900", "950", "1000", "1050", "1100"}


def test_downlink_is_written_as_the_shortest_decimal() -> None:
    assert {browser_downlink("foo.com", 10_000, salt) for salt in BROWSER_SALTS} == {
        "9",
        "9.1",
        "9.2",
        "9.3",
        "9.4",
        "9.5",
        "9.6",
        "9.7",
        "9.8",
        "9.9",
        "10",
    }


def test_the_salt_of_a_renderer_is_from_1_to_20_and_differs_by_site() -> None:
    salts = {renderer_salt("key", f"https://site{number}.test") for number in range(200)}
    assert salts == set(RENDERER_SALTS)
    assert renderer_salt("key", "https://example.test") == renderer_salt("key", "https://example.test")


def _rtts(host: str, round_trip_ms: int) -> list[int]:
    return [int(browser_rtt(host, round_trip_ms, salt)) for salt in BROWSER_SALTS] + [
        int(renderer_rtt(host, round_trip_ms, salt)) for salt in RENDERER_SALTS
    ]


def _downlinks(host: str, throughput_kbps: int) -> list[float]:
    return [float(browser_downlink(host, throughput_kbps, salt)) for salt in BROWSER_SALTS] + [
        float(renderer_downlink(host, throughput_kbps, salt)) for salt in RENDERER_SALTS
    ]
