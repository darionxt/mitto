"""Names for the numbers in a ClientHello and in HTTP/2 frames, so that a normal form reads without a registry at hand.

A number with no name here keeps its hexadecimal form. Giving one a name later changes the normal forms that hold it,
so those have to be recorded again.
"""

GREASE = "GREASE"
"""What stands for a GREASE value. Clients pick those at random to keep servers from choking on unknown values."""


def _is_grease_u16(value: int) -> bool:
    """Whether value is one of the two-byte GREASE values: both bytes the same, and ending in the digit A."""
    return value & 0x0F0F == 0x0A0A and value >> 8 == value & 0xFF


def _named_u16(names: dict[int, str], value: int) -> str:
    if _is_grease_u16(value):
        return GREASE
    return names.get(value, f"0x{value:04x}")


VERSIONS = {
    0x0301: "TLS 1.0",
    0x0302: "TLS 1.1",
    0x0303: "TLS 1.2",
    0x0304: "TLS 1.3",
}

CIPHER_SUITES = {
    0x002F: "TLS_RSA_WITH_AES_128_CBC_SHA",
    0x0035: "TLS_RSA_WITH_AES_256_CBC_SHA",
    0x009C: "TLS_RSA_WITH_AES_128_GCM_SHA256",
    0x009D: "TLS_RSA_WITH_AES_256_GCM_SHA384",
    0x1301: "TLS_AES_128_GCM_SHA256",
    0x1302: "TLS_AES_256_GCM_SHA384",
    0x1303: "TLS_CHACHA20_POLY1305_SHA256",
    0xC013: "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA",
    0xC014: "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA",
    0xC02B: "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
    0xC02C: "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
    0xC02F: "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
    0xC030: "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
    0xCCA8: "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
    0xCCA9: "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
}

GROUPS = {
    0x0017: "secp256r1",
    0x0018: "secp384r1",
    0x0019: "secp521r1",
    0x001D: "x25519",
    0x001E: "x448",
    0x11EB: "SecP256r1MLKEM768",
    0x11EC: "X25519MLKEM768",
    0x11ED: "SecP384r1MLKEM1024",
    0x6399: "X25519Kyber768Draft00",
}

SIGNATURE_ALGORITHMS = {
    0x0201: "rsa_pkcs1_sha1",
    0x0203: "ecdsa_sha1",
    0x0401: "rsa_pkcs1_sha256",
    0x0403: "ecdsa_secp256r1_sha256",
    0x0501: "rsa_pkcs1_sha384",
    0x0503: "ecdsa_secp384r1_sha384",
    0x0601: "rsa_pkcs1_sha512",
    0x0603: "ecdsa_secp521r1_sha512",
    0x0804: "rsa_pss_rsae_sha256",
    0x0805: "rsa_pss_rsae_sha384",
    0x0806: "rsa_pss_rsae_sha512",
    0x0807: "ed25519",
    0x0808: "ed448",
    0x0809: "rsa_pss_pss_sha256",
    0x080A: "rsa_pss_pss_sha384",
    0x080B: "rsa_pss_pss_sha512",
    0x0904: "mldsa44",
    0x0905: "mldsa65",
    0x0906: "mldsa87",
}

COMPRESSION_METHODS = {0: "null"}

EC_POINT_FORMATS = {0: "uncompressed"}

PSK_KEY_EXCHANGE_MODES = {0: "psk_ke", 1: "psk_dhe_ke"}

CERTIFICATE_COMPRESSION = {1: "zlib", 2: "brotli", 3: "zstd"}

ECH_KDFS = {1: "HKDF-SHA256", 2: "HKDF-SHA384", 3: "HKDF-SHA512"}

ECH_AEADS = {1: "AES-128-GCM", 2: "AES-256-GCM", 3: "ChaCha20Poly1305"}

ECH_CLIENT_HELLO_TYPES = {0: "outer", 1: "inner"}

EXTENSIONS = {
    0: "server_name",
    5: "status_request",
    10: "supported_groups",
    11: "ec_point_formats",
    13: "signature_algorithms",
    16: "application_layer_protocol_negotiation",
    18: "signed_certificate_timestamp",
    21: "padding",
    23: "extended_master_secret",
    27: "compress_certificate",
    28: "record_size_limit",
    34: "delegated_credentials",
    35: "session_ticket",
    41: "pre_shared_key",
    42: "early_data",
    43: "supported_versions",
    44: "cookie",
    45: "psk_key_exchange_modes",
    49: "post_handshake_auth",
    50: "signature_algorithms_cert",
    51: "key_share",
    57: "quic_transport_parameters",
    17513: "application_settings_old",
    17613: "application_settings",
    51764: "trust_anchors",
    65037: "encrypted_client_hello",
    65281: "renegotiation_info",
}

FRAME_TYPES = {
    0x0: "DATA",
    0x1: "HEADERS",
    0x2: "PRIORITY",
    0x3: "RST_STREAM",
    0x4: "SETTINGS",
    0x5: "PUSH_PROMISE",
    0x6: "PING",
    0x7: "GOAWAY",
    0x8: "WINDOW_UPDATE",
    0x9: "CONTINUATION",
    0xA: "ALTSVC",
    0xC: "ORIGIN",
    0x10: "PRIORITY_UPDATE",
}

HTTP2_SETTINGS = {
    0x1: "HEADER_TABLE_SIZE",
    0x2: "ENABLE_PUSH",
    0x3: "MAX_CONCURRENT_STREAMS",
    0x4: "INITIAL_WINDOW_SIZE",
    0x5: "MAX_FRAME_SIZE",
    0x6: "MAX_HEADER_LIST_SIZE",
    0x8: "ENABLE_CONNECT_PROTOCOL",
    0x9: "NO_RFC7540_PRIORITIES",
}

FRAME_FLAGS = {
    0x0: {0x1: "END_STREAM", 0x8: "PADDED"},
    0x1: {0x1: "END_STREAM", 0x4: "END_HEADERS", 0x8: "PADDED", 0x20: "PRIORITY"},
    0x4: {0x1: "ACK"},
    0x6: {0x1: "ACK"},
    0x9: {0x4: "END_HEADERS"},
}


def version_name(value: int) -> str:
    return _named_u16(VERSIONS, value)


def cipher_suite_name(value: int) -> str:
    return _named_u16(CIPHER_SUITES, value)


def group_name(value: int) -> str:
    return _named_u16(GROUPS, value)


def signature_algorithm_name(value: int) -> str:
    return _named_u16(SIGNATURE_ALGORITHMS, value)


def extension_name(value: int) -> str:
    return _named_u16(EXTENSIONS, value)


def ech_kdf_name(value: int) -> str:
    return ECH_KDFS.get(value, f"0x{value:04x}")


def ech_aead_name(value: int) -> str:
    return ECH_AEADS.get(value, f"0x{value:04x}")


def one_byte_name(names: dict[int, str], value: int) -> str:
    """Name a one-byte value from a registry with no GREASE values of this form."""
    return names.get(value, f"0x{value:02x}")


def certificate_compression_name(value: int) -> str:
    return CERTIFICATE_COMPRESSION.get(value, f"0x{value:04x}")


def frame_type_name(value: int) -> str:
    """Name an HTTP/2 frame type. GREASE frame types are 0x0b and every 0x1f after it."""
    if value >= 0x0B and (value - 0x0B) % 0x1F == 0:
        return GREASE
    return FRAME_TYPES.get(value, f"0x{value:02x}")


def http2_setting_name(value: int) -> str:
    """Name an HTTP/2 setting. GREASE settings end in the digit A in both bytes, but the bytes may differ."""
    if value & 0x0F0F == 0x0A0A:
        return GREASE
    return HTTP2_SETTINGS.get(value, f"0x{value:04x}")


def frame_flag_names(frame_type: int, flags: int) -> list[str]:
    """Name a frame's flags, lowest bit first. A flag this frame type does not define keeps its hexadecimal form."""
    names = FRAME_FLAGS.get(frame_type, {})
    return [names.get(1 << bit, f"0x{1 << bit:02x}") for bit in range(8) if flags & (1 << bit)]
