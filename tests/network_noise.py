"""Chrome's noise on rtt and downlink for each host, written again in Python, so that tests know what Chrome sends.

The native core's network_quality.cc ports it from content/browser/client_hints/client_hints.cc, where the browser process
works out a navigation's values with libc++'s std::hash, and from
third_party/blink/renderer/platform/network/network_state_notifier.cc, where the renderer process of a page works out a
subresource's with Blink's hash of a string. This follows the same files and libc++'s __functional/hash.h, so a test that
compares the two compares ports that share nothing but Chromium's source. The native core calls the std::hash of the libc++
that Chrome builds with, so such a test also checks the CityHash64 here.
"""

import hashlib
import math

__all__ = [
    "blink_string_hash",
    "browser_downlink",
    "browser_rtt",
    "libcxx_string_hash",
    "renderer_downlink",
    "renderer_rtt",
    "renderer_salt",
]

_MASK = (1 << 64) - 1

# Some primes between 2^63 and 2^64, in libc++'s __murmur2_or_cityhash<size_t, 64>.
_K0 = 0xC3A5C85C97CB3127
_K1 = 0xB492B66FBE98F273
_K2 = 0x9AE16A3B2F90404F
_K3 = 0xC949D7C7509E6557

# third_party/rapidhash/rapidhash.h.
_RAPID_SEED = 0xBDD89AA982704029
_RAPID_SECRET = (0x2D358DCCAA6C78A5, 0x8BB84B93962EACC9, 0x4B33A62ED433D4A3)

# GetRandomMultiplier() in both places: one of 21 multipliers from 0.90 to 1.10.
_MULTIPLIERS = 21

# What RoundRtt() caps at and rounds to, in microseconds, which base::TimeDelta holds.
_MAX_RTT_US = 3_000_000
_RTT_GRANULARITY_US = 50_000

# What RoundKbpsToMbps() and RoundMbps() cap at and round to, in kbps.
_MAX_DOWNLINK_KBPS = 10.0 * 1000
_DOWNLINK_GRANULARITY_KBPS = 50

# The renderer's salt is from 1 to 20.
_RENDERER_SALTS = 20


def _load64(data: bytes, index: int) -> int:
    return int.from_bytes(data[index : index + 8], "little")


def _load32(data: bytes, index: int) -> int:
    return int.from_bytes(data[index : index + 4], "little")


# libc++'s std::hash<std::string>: CityHash64 where size_t has 64 bits, with _LIBCPP_ABI_FIX_CITYHASH_IMPLEMENTATION, which
# the _LIBCPP_ABI_VERSION 2 of Chrome's libc++ defines.


def _rotate(value: int, shift: int) -> int:
    return value if shift == 0 else ((value >> shift) | (value << (64 - shift))) & _MASK


def _shift_mix(value: int) -> int:
    return value ^ (value >> 47)


def _hash_len_16(u: int, v: int) -> int:
    mul = 0x9DDFEA08EB382D69
    a = ((u ^ v) * mul) & _MASK
    a ^= a >> 47
    b = ((v ^ a) * mul) & _MASK
    b ^= b >> 47
    return (b * mul) & _MASK


def _hash_len_0_to_16(data: bytes) -> int:
    length = len(data)
    if length > 8:
        a = _load64(data, 0)
        b = _load64(data, length - 8)
        return _hash_len_16(a, _rotate((b + length) & _MASK, length)) ^ b
    if length >= 4:
        # The fix widens the first word before it shifts.
        return _hash_len_16(length + (_load32(data, 0) << 3), _load32(data, length - 4))
    if length > 0:
        y = (data[0] + (data[length >> 1] << 8)) & 0xFFFFFFFF
        z = (length + (data[length - 1] << 2)) & 0xFFFFFFFF
        return (_shift_mix(((y * _K2) ^ (z * _K3)) & _MASK) * _K2) & _MASK
    return _K2


def _hash_len_17_to_32(data: bytes) -> int:
    length = len(data)
    a = (_load64(data, 0) * _K1) & _MASK
    b = _load64(data, 8)
    c = (_load64(data, length - 8) * _K2) & _MASK
    d = (_load64(data, length - 16) * _K0) & _MASK
    return _hash_len_16(
        (_rotate((a - b) & _MASK, 43) + _rotate(c, 30) + d) & _MASK,
        (a + _rotate(b ^ _K3, 20) - c + length) & _MASK,
    )


def _weak_hash_len_32_with_seeds(data: bytes, index: int, a: int, b: int) -> tuple[int, int]:
    w, x, y, z = (_load64(data, index + offset) for offset in (0, 8, 16, 24))
    a = (a + w) & _MASK
    b = _rotate((b + a + z) & _MASK, 21)
    c = a
    a = (a + x + y) & _MASK
    b = (b + _rotate(a, 44)) & _MASK
    return (a + z) & _MASK, (b + c) & _MASK


def _hash_len_33_to_64(data: bytes) -> int:
    length = len(data)
    z = _load64(data, 24)
    a = (_load64(data, 0) + (length + _load64(data, length - 16)) * _K0) & _MASK
    b = _rotate((a + z) & _MASK, 52)
    c = _rotate(a, 37)
    a = (a + _load64(data, 8)) & _MASK
    c = (c + _rotate(a, 7)) & _MASK
    a = (a + _load64(data, 16)) & _MASK
    vf = (a + z) & _MASK
    vs = (b + _rotate(a, 31) + c) & _MASK
    a = (_load64(data, 16) + _load64(data, length - 32)) & _MASK
    z = (z + _load64(data, length - 8)) & _MASK
    b = _rotate((a + z) & _MASK, 52)
    c = _rotate(a, 37)
    a = (a + _load64(data, length - 24)) & _MASK
    c = (c + _rotate(a, 7)) & _MASK
    a = (a + _load64(data, length - 16)) & _MASK
    wf = (a + z) & _MASK
    ws = (b + _rotate(a, 31) + c) & _MASK
    r = _shift_mix(((vf + ws) * _K2 + (wf + vs) * _K0) & _MASK)
    return (_shift_mix((r * _K0 + vs) & _MASK) * _K2) & _MASK


def libcxx_string_hash(data: bytes) -> int:
    """Return what libc++'s std::hash<std::string> gives these bytes in a 64-bit Chrome."""
    length = len(data)
    if length <= 16:
        return _hash_len_0_to_16(data)
    if length <= 32:
        return _hash_len_17_to_32(data)
    if length <= 64:
        return _hash_len_33_to_64(data)

    # The end first, and then 64 bytes at a time with 56 bytes of state.
    x = _load64(data, length - 40)
    y = (_load64(data, length - 16) + _load64(data, length - 56)) & _MASK
    z = _hash_len_16((_load64(data, length - 48) + length) & _MASK, _load64(data, length - 24))
    v = _weak_hash_len_32_with_seeds(data, length - 64, length, z)
    w = _weak_hash_len_32_with_seeds(data, length - 32, (y + _K1) & _MASK, x)
    x = (x * _K1 + _load64(data, 0)) & _MASK
    position = 0
    for _ in range((length - 1) // 64):
        x = (_rotate((x + y + v[0] + _load64(data, position + 8)) & _MASK, 37) * _K1) & _MASK
        y = (_rotate((y + v[1] + _load64(data, position + 48)) & _MASK, 42) * _K1) & _MASK
        x ^= w[1]
        y = (y + v[0] + _load64(data, position + 40)) & _MASK
        z = (_rotate((z + w[0]) & _MASK, 33) * _K1) & _MASK
        v = _weak_hash_len_32_with_seeds(data, position, (v[1] * _K1) & _MASK, (x + w[0]) & _MASK)
        w = _weak_hash_len_32_with_seeds(
            data, position + 32, (z + w[1]) & _MASK, (y + _load64(data, position + 16)) & _MASK
        )
        z, x = x, z
        position += 64
    return _hash_len_16(
        (_hash_len_16(v[0], w[0]) + _shift_mix(y) * _K1 + z) & _MASK, (_hash_len_16(v[1], w[1]) + x) & _MASK
    )


# Blink's StringImpl::GetHash() of 8-bit characters: rapidhash with PlainHashReader, cut to 24 bits.


def _rapid_mul128(a: int, b: int) -> tuple[int, int]:
    product = a * b
    return product & _MASK, product >> 64


def _rapid_mix(a: int, b: int) -> int:
    low, high = _rapid_mul128(a, b)
    return low ^ high


def rapidhash(data: bytes) -> int:
    """Return rapidhash of these bytes, with its default seed and secrets."""
    secret = _RAPID_SECRET
    length = len(data)
    seed = _RAPID_SEED ^ _rapid_mix(_RAPID_SEED ^ secret[0], secret[1]) ^ length
    if length <= 16:
        if length >= 4:
            last = length - 4
            delta = (length & 24) >> (length >> 3)
            a = (_load32(data, 0) << 32) | _load32(data, last)
            b = (_load32(data, delta) << 32) | _load32(data, last - delta)
        elif length > 0:
            a = (data[0] << 56) | (data[length >> 1] << 32) | data[length - 1]
            b = 0
        else:
            a = b = 0
    else:
        position = 0
        left = length
        if left > 48:
            see1 = see2 = seed
            while True:
                seed = _rapid_mix(_load64(data, position) ^ secret[0], _load64(data, position + 8) ^ seed)
                see1 = _rapid_mix(_load64(data, position + 16) ^ secret[1], _load64(data, position + 24) ^ see1)
                see2 = _rapid_mix(_load64(data, position + 32) ^ secret[2], _load64(data, position + 40) ^ see2)
                position += 48
                left -= 48
                if left < 48:
                    break
            seed ^= see1 ^ see2
        if left > 16:
            seed = _rapid_mix(_load64(data, position) ^ secret[2], _load64(data, position + 8) ^ seed ^ secret[1])
            if left > 32:
                seed = _rapid_mix(_load64(data, position + 16) ^ secret[2], _load64(data, position + 24) ^ seed)
        a = _load64(data, position + left - 16)
        b = _load64(data, position + left - 8)
    a, b = _rapid_mul128(a ^ secret[1], b ^ seed)
    return _rapid_mix(a ^ secret[0] ^ length, b ^ secret[1])


def blink_string_hash(data: bytes) -> int:
    """Return what Blink's StringImpl::GetHash() gives a string of these 8-bit characters."""
    hash_value = rapidhash(data) & ((1 << 24) - 1)
    return hash_value or 0x80000000 >> 8


# The noise and the rounding.


def _multiplier(hash_value: int) -> float:
    return 0.9 + float(hash_value % _MULTIPLIERS) * 0.01


def _browser_multiplier(host: str, salt: int) -> float:
    # unsigned hash = std::hash<std::string>{}(host) + RandomizationSalt(), which cuts the sum to 32 bits.
    return _multiplier((libcxx_string_hash(host.encode()) + salt) & 0xFFFFFFFF)


def _renderer_multiplier(host: str, salt: int) -> float:
    return _multiplier(blink_string_hash(host.encode()) + salt)


def _round_half_away(value: float) -> int:
    """Return std::round() of a number of at least 0, which takes a half away from zero."""
    floor = math.floor(value)
    return floor + 1 if value - floor >= 0.5 else floor


def _round_rtt(round_trip_ms: int, multiplier: float) -> int:
    # TimeDelta * double drops the fraction of a microsecond, and RoundToMultiple() takes a half up.
    modified_us = min(int(round_trip_ms * 1000 * multiplier), _MAX_RTT_US)
    rounded_us = (modified_us + _RTT_GRANULARITY_US // 2) // _RTT_GRANULARITY_US * _RTT_GRANULARITY_US
    return rounded_us // 1000


def _round_downlink(throughput_kbps: float, multiplier: float) -> float:
    capped = min(throughput_kbps * multiplier, _MAX_DOWNLINK_KBPS)
    return _round_half_away(capped / _DOWNLINK_GRANULARITY_KBPS) * _DOWNLINK_GRANULARITY_KBPS / 1000


def _number_text(number: float) -> str:
    """Return a number as base::NumberToString() writes it: the shortest decimal, with no point for a whole number."""
    return str(int(number)) if number.is_integer() else repr(number)


def browser_rtt(host: str, round_trip_ms: int, salt: int) -> str:
    """Return the rtt that the browser process with this salt sends on a navigation's hop to `host`."""
    return str(_round_rtt(round_trip_ms, _browser_multiplier(host, salt)))


def browser_downlink(host: str, throughput_kbps: int, salt: int) -> str:
    """Return the downlink that the browser process with this salt sends on a navigation's hop to `host`."""
    return _number_text(_round_downlink(float(throughput_kbps), _browser_multiplier(host, salt)))


def renderer_rtt(host: str, round_trip_ms: int, salt: int) -> str:
    """Return the rtt that a renderer process with this salt sends on a subresource that starts at `host`."""
    return str(_round_rtt(round_trip_ms, _renderer_multiplier(host, salt)))


def renderer_downlink(host: str, throughput_kbps: int, salt: int) -> str:
    """Return the downlink that a renderer process with this salt sends on a subresource that starts at `host`."""
    # The renderer holds the throughput in Mbps, and RoundMbps() takes it back to kbps.
    throughput_mbps = float(throughput_kbps) / 1000
    return _number_text(_round_downlink(throughput_mbps * 1000, _renderer_multiplier(host, salt)))


def renderer_salt(renderer_key: str, site: str) -> int:
    """Return the salt that the native core works out for the renderer process of a site, written as its scheme and
    registrable domain, such as "https://example.test"."""
    digest = hashlib.sha256(f"{renderer_key} {site}".encode()).digest()
    return 1 + int.from_bytes(digest[:8], "big") % _RENDERER_SALTS
