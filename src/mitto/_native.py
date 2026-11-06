"""The private binding of the native core's C API.

Nothing is loaded until load_library() is called, so the package can be imported on a machine without the native
library. Session is the only way to reach the engine settings and per-request headers, which stay private until the
public Session API exists.
"""

import ctypes
import enum
import functools
import json
import os
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Self

__all__ = [
    "ABI_VERSION",
    "LIBRARY_ENV_VAR",
    "LIBRARY_NAME",
    "SETTINGS_VERSION",
    "MittoHeader",
    "MittoResponse",
    "MittoSession",
    "NativeLibraryError",
    "NativeResponse",
    "Priority",
    "RequestError",
    "Session",
    "encode_url",
    "find_library",
    "load_library",
]

# The MITTO_ABI_VERSION of the C header this module is written against. Change it together with the declarations below.
ABI_VERSION = 3

# The MITTO_SETTINGS_VERSION of the C header this module is written against.
SETTINGS_VERSION = 1

# Names the library of a local build. It takes precedence over the library in the installed package.
LIBRARY_ENV_VAR = "MITTO_NATIVE_LIBRARY"

LIBRARY_NAME = "libmitto_core.dylib" if sys.platform == "darwin" else "libmitto_core.so"


class NativeLibraryError(RuntimeError):
    """The native library is missing, cannot be loaded or was built for another ABI version."""


class RequestError(Exception):
    """A request ended without a response.

    Attributes:
        url: The URL that was requested.
        net_error: The negative Chromium net error code, such as -105 for ERR_NAME_NOT_RESOLVED.
    """

    def __init__(self, url: str, net_error: int) -> None:
        # Exception pickles its args, so they have to be what __init__ takes.
        super().__init__(url, net_error)
        self.url = url
        self.net_error = net_error

    def __str__(self) -> str:
        return f"GET {self.url} failed with net error {self.net_error}"


class Priority(enum.IntEnum):
    """A request's priority, with the values of the C header's MITTO_PRIORITY constants."""

    THROTTLED = 0
    IDLE = 1
    LOWEST = 2
    LOW = 3
    MEDIUM = 4
    HIGHEST = 5


class MittoSession(ctypes.Structure):
    """The opaque type behind a Session handle in the C API."""


class MittoHeader(ctypes.Structure):
    """One request header, laid out like the C struct of the same name."""

    _fields_ = (
        ("name", ctypes.c_char_p),
        ("name_size", ctypes.c_size_t),
        ("value", ctypes.c_char_p),
        ("value_size", ctypes.c_size_t),
    )


class MittoResponse(ctypes.Structure):
    """A finished response, laid out like the C struct of the same name.

    The native core owns the buffers. Copy what is needed, then release them with mitto_response_free().
    """

    _fields_ = (
        ("status", ctypes.c_int32),
        ("protocol", ctypes.c_char_p),
        ("headers", ctypes.POINTER(ctypes.c_char)),
        ("headers_size", ctypes.c_size_t),
        ("body", ctypes.POINTER(ctypes.c_uint8)),
        ("body_size", ctypes.c_size_t),
    )


def find_library() -> Path | None:
    """Return the path of the native library to load, or None if there is none.

    Raises:
        NativeLibraryError: If LIBRARY_ENV_VAR names something that is not a file. Falling back to another library
            would hide that mistake.
    """
    if override := os.environ.get(LIBRARY_ENV_VAR):
        # An absolute path also stops dlopen() from searching for a bare file name.
        path = Path(override).resolve()
        if not path.is_file():
            raise NativeLibraryError(f"{LIBRARY_ENV_VAR} names {path}, which is not a file")
        return path
    path = Path(__file__).with_name(LIBRARY_NAME)
    return path if path.is_file() else None


@functools.cache
def load_library() -> ctypes.CDLL:
    """Load the native library, check its ABI version and declare the types of its functions.

    The library is loaded once per process. Its functions release the GIL while they run, as all ctypes.CDLL functions
    do, so a blocking call does not stop other Python threads.

    Raises:
        NativeLibraryError: If there is no library, it cannot be loaded or it has another ABI version.
    """
    path = find_library()
    if path is None:
        raise NativeLibraryError(
            f"the installed package has no {LIBRARY_NAME}; set {LIBRARY_ENV_VAR} to the path of a local build"
        )

    try:
        library = ctypes.CDLL(path)

        library.mitto_abi_version.argtypes = ()
        library.mitto_abi_version.restype = ctypes.c_uint32
        # This comes first, because a library with another ABI version may not export what is declared below.
        version: int = library.mitto_abi_version()
        if version != ABI_VERSION:
            raise NativeLibraryError(f"{path} has ABI version {version}, but this package needs {ABI_VERSION}")

        session_pointer = ctypes.POINTER(MittoSession)
        library.mitto_session_create.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(session_pointer))
        library.mitto_session_create.restype = ctypes.c_int32
        library.mitto_session_destroy.argtypes = (session_pointer,)
        library.mitto_session_destroy.restype = None
        library.mitto_session_request.argtypes = (
            session_pointer,
            ctypes.c_char_p,
            ctypes.POINTER(MittoHeader),
            ctypes.c_size_t,
            ctypes.c_int32,
            ctypes.c_bool,
            ctypes.c_char_p,
            ctypes.POINTER(MittoResponse),
        )
        library.mitto_session_request.restype = ctypes.c_int32
        library.mitto_response_free.argtypes = (ctypes.POINTER(MittoResponse),)
        library.mitto_response_free.restype = None
    except (AttributeError, OSError) as error:
        raise NativeLibraryError(f"cannot use {path} as the native library: {error}") from error

    return library


def encode_url(url: str) -> bytes:
    """Return `url` the way the C API takes it.

    Raises:
        ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8.
    """
    encoded_url = url.encode()
    # The C API takes a NUL-terminated string, so it would fetch only what comes before the NUL.
    if b"\0" in encoded_url:
        raise ValueError("url holds a NUL character")
    return encoded_url


@dataclass(frozen=True, slots=True)
class NativeResponse:
    """A copy of a finished response from the native core.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        protocol: The protocol the response arrived over, for example "http/1.1" or "h2".
        headers: The final response's (name, value) pairs in the order received, so repeated headers stay separate.
            Headers have no fixed encoding, so they are decoded as Latin-1, which keeps every byte:
            value.encode("latin-1") gives back what the server sent.
        body: The final response's body, after any content decoding. It stays out of repr(), since it can be large.
    """

    status: int
    protocol: str
    headers: tuple[tuple[str, str], ...]
    body: bytes = field(repr=False)


class Session:
    """A Session in the native core, with its own request context.

    One Session may run requests from several threads at once, but must not be closed while one is running.
    """

    def __init__(
        self,
        *,
        ignore_certificate_errors_spki_list: Sequence[str] = (),
        accept_language: str = "",
        origins_to_force_quic_on: Sequence[str] = (),
        dns_nameserver: str | None = None,
    ) -> None:
        """Create the Session.

        Args:
            ignore_certificate_errors_spki_list: Accept any certificate chain that holds one of these keys, with the
                meaning of Chrome's --ignore-certificate-errors-spki-list. Each entry is the Base64 form of the SHA-256
                hash of a DER-encoded SubjectPublicKeyInfo.
            accept_language: The value of the Accept-Language header that //net adds to each request whose headers do not
                hold one. An empty string adds no header.
            origins_to_force_quic_on: Reach these HTTPS origins, each written "host:port", over QUIC and nothing else,
                with the meaning of Chrome's --origin-to-force-quic-on. It also lifts //net's refusal of QUIC to a
                certificate from an unknown root, for each of their hosts. The certificate still has to verify. "*"
                stands for every origin. Without any, a request learns of QUIC from Alt-Svc.
            dns_nameserver: Ask only this DNS server, written "address:port" with an IPv6 address in brackets, in place of
                what the system's settings name. It is for tests, so the public API does not offer it.

        Raises:
            ValueError: If the native core refuses the settings.
            NativeLibraryError: If the native library cannot be used.
        """
        self._library = load_library()
        settings_dict: dict[str, object] = {
            "version": SETTINGS_VERSION,
            "ignore_certificate_errors_spki_list": list(ignore_certificate_errors_spki_list),
            "accept_language": accept_language,
            "origins_to_force_quic_on": list(origins_to_force_quic_on),
        }
        if dns_nameserver is not None:
            settings_dict["dns_nameserver"] = dns_nameserver
        settings = json.dumps(settings_dict).encode()
        self._handle = ctypes.POINTER(MittoSession)()
        net_error: int = self._library.mitto_session_create(settings, len(settings), ctypes.byref(self._handle))
        if net_error != 0:
            raise ValueError(f"the native core refused the settings {settings!r} with net error {net_error}")

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    def close(self) -> None:
        """Close the Session's connections and release it. Later requests fail with a RequestError."""
        self._library.mitto_session_destroy(self._handle)
        self._handle = ctypes.POINTER(MittoSession)()

    def request(
        self,
        url: str,
        headers: Sequence[tuple[str, str]] = (),
        # The value of DEFAULT_PRIORITY in //net.
        priority: Priority = Priority.LOWEST,
        # The value of kDefaultPriorityIncremental in //net.
        priority_incremental: bool = True,
        referrer: str | None = None,
    ) -> NativeResponse:
        """Fetch `url` and block until the whole response has arrived.

        Args:
            url: The URL to fetch.
            headers: The (name, value) pairs to send, in the order they are to reach the wire, encoded as Latin-1. A
                name may appear only once, ignoring case. //net adds its own headers as it does in Chrome, such as Host
                before these and Accept-Encoding after them, unless they are among these. Referer cannot be among these,
                because //net only sends `referrer`.
            priority: The priority of the request.
            priority_incremental: Whether the response can be used as it arrives. With `priority`, this decides the
                HTTP/2 weight, the priority header and the HTTP/3 PRIORITY_UPDATE.
            referrer: The URL to send as the Referer header, or None to send none. //net puts it after `headers` and
                before the headers it adds, and leaves out any username, password and fragment.

        Raises:
            ValueError: If `url` or `referrer` holds a NUL character or cannot be encoded as UTF-8, or a header cannot be
                encoded as Latin-1.
            RequestError: If the request ends without a response. An HTTP error status such as 404 is a response. An
                invalid header, or a `referrer` that //net cannot send, ends the request with ERR_INVALID_ARGUMENT.
        """
        encoded_url = encode_url(url)
        encoded_referrer = None if referrer is None else encode_url(referrer)
        # This list keeps the bytes alive during the call, since the structs only point to them.
        encoded_headers = [(name.encode("latin-1"), value.encode("latin-1")) for name, value in headers]
        native_headers = (MittoHeader * len(encoded_headers))(
            *(MittoHeader(name, len(name), value, len(value)) for name, value in encoded_headers)
        )

        native = MittoResponse()
        net_error: int = self._library.mitto_session_request(
            self._handle,
            encoded_url,
            native_headers,
            len(native_headers),
            priority,
            priority_incremental,
            encoded_referrer,
            ctypes.byref(native),
        )
        if net_error != 0:
            raise RequestError(url, net_error)
        try:
            return NativeResponse(
                status=native.status,
                protocol=native.protocol.decode(),
                headers=_parse_headers(ctypes.string_at(native.headers, native.headers_size)),
                body=ctypes.string_at(native.body, native.body_size),
            )
        finally:
            self._library.mitto_response_free(ctypes.byref(native))


def _parse_headers(block: bytes) -> tuple[tuple[str, str], ...]:
    """Split the C API's "name: value\\r\\n" lines into pairs.

    Only CRLF ends a line. str.splitlines() would also split on characters that a value may hold, such as 0x85.
    """
    lines = block.decode("latin-1").split("\r\n")
    return tuple((name, value) for name, _, value in (line.partition(": ") for line in lines if line))
