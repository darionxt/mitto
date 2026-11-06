"""The private binding of the native core's C API.

Nothing is loaded until load_library() is called, so the package can be imported on a machine without the native
library. The public Session is built on the Session here, and the settings that only tests need stay private.
"""

import ctypes
import enum
import functools
import json
import math
import os
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Any, Self, TypedDict

from mitto._errors import NativeLibraryError, RequestError, request_error
from mitto._timeout import NO_TIMEOUT, Timeout

__all__ = [
    "ABI_VERSION",
    "LIBRARY_ENV_VAR",
    "LIBRARY_NAME",
    "REQUEST_OPTIONS_VERSION",
    "SETTINGS_VERSION",
    "BodyAvailable",
    "EventType",
    "Finished",
    "MittoEvent",
    "MittoHeader",
    "MittoRequest",
    "MittoSession",
    "NativeEvent",
    "NativeLibraryError",
    "NativeRequest",
    "NativeResponse",
    "Priority",
    "PrivateSettings",
    "RedirectFollowed",
    "RequestError",
    "ResponseStarted",
    "Session",
    "encode_url",
    "find_library",
    "load_library",
    "timeout_options",
]

# The MITTO_ABI_VERSION of the C header this module is written against. Change it together with the declarations below.
ABI_VERSION = 4

# The MITTO_SETTINGS_VERSION of the C header this module is written against.
SETTINGS_VERSION = 1

# The MITTO_REQUEST_OPTIONS_VERSION of the C header this module is written against.
REQUEST_OPTIONS_VERSION = 1

# Names the library of a local build. It takes precedence over the library in the installed package.
LIBRARY_ENV_VAR = "MITTO_NATIVE_LIBRARY"

LIBRARY_NAME = "libmitto_core.dylib" if sys.platform == "darwin" else "libmitto_core.so"

# What the C API returns when there is nothing yet.
_ERR_IO_PENDING = -1

# How much a read asks for: as much as the native core holds at a time.
_READ_SIZE = 64 * 1024


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


class MittoRequest(ctypes.Structure):
    """The opaque type behind a request handle in the C API."""


class EventType(enum.IntEnum):
    """The type of a request's event, with the values of the C header's MITTO_EVENT constants."""

    RESPONSE_STARTED = 1
    REDIRECT_FOLLOWED = 2
    BODY_AVAILABLE = 3
    FINISHED = 4


class MittoHeader(ctypes.Structure):
    """One request header, laid out like the C struct of the same name."""

    _fields_ = (
        ("name", ctypes.c_char_p),
        ("name_size", ctypes.c_size_t),
        ("value", ctypes.c_char_p),
        ("value_size", ctypes.c_size_t),
    )


class MittoEvent(ctypes.Structure):
    """One event of a request, laid out like the C struct of the same name.

    The native core owns the buffers. Copy what is needed, then release them with mitto_event_free().
    """

    _fields_ = (
        ("type", ctypes.c_int32),
        ("status", ctypes.c_int32),
        ("net_error", ctypes.c_int32),
        ("protocol", ctypes.c_char_p),
        ("url", ctypes.c_char_p),
        ("headers", ctypes.POINTER(ctypes.c_char)),
        ("headers_size", ctypes.c_size_t),
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

        library.mitto_net_error_name.argtypes = (ctypes.c_int32,)
        library.mitto_net_error_name.restype = ctypes.c_char_p
        session_pointer = ctypes.POINTER(MittoSession)
        request_pointer = ctypes.POINTER(MittoRequest)
        library.mitto_session_create.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(session_pointer))
        library.mitto_session_create.restype = ctypes.c_int32
        library.mitto_session_destroy.argtypes = (session_pointer,)
        library.mitto_session_destroy.restype = None
        library.mitto_session_event_fd.argtypes = (session_pointer, ctypes.POINTER(ctypes.c_int32))
        library.mitto_session_event_fd.restype = ctypes.c_int32
        library.mitto_session_next_ready_request.argtypes = (session_pointer, ctypes.POINTER(request_pointer))
        library.mitto_session_next_ready_request.restype = ctypes.c_int32
        library.mitto_request_start.argtypes = (
            session_pointer,
            ctypes.c_char_p,
            ctypes.POINTER(MittoHeader),
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(request_pointer),
        )
        library.mitto_request_start.restype = ctypes.c_int32
        library.mitto_request_next_event.argtypes = (request_pointer, ctypes.c_int64, ctypes.POINTER(MittoEvent))
        library.mitto_request_next_event.restype = ctypes.c_int32
        library.mitto_request_read.argtypes = (request_pointer, ctypes.c_char_p, ctypes.c_size_t)
        library.mitto_request_read.restype = ctypes.c_int32
        library.mitto_request_cancel.argtypes = (request_pointer,)
        library.mitto_request_cancel.restype = None
        library.mitto_request_release.argtypes = (request_pointer,)
        library.mitto_request_release.restype = None
        library.mitto_event_free.argtypes = (ctypes.POINTER(MittoEvent),)
        library.mitto_event_free.restype = None
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


def timeout_options(timeout: Timeout) -> dict[str, int]:
    """Return the timers that are set, as the options of the C API's mitto_request_start()."""
    timers = {"connect_timeout_ms": timeout.connect, "read_timeout_ms": timeout.read, "total_timeout_ms": timeout.total}
    return {key: math.ceil(seconds * 1000) for key, seconds in timers.items() if seconds is not None}


def _request_error(library: ctypes.CDLL, url: str, net_error: int, method: str) -> RequestError:
    """Return the error for a net error, with its name from the native core, which spells it as the Engine Release does."""
    name: bytes = library.mitto_net_error_name(net_error)
    return request_error(url, net_error, method, name.decode())


@dataclass(frozen=True, slots=True)
class ResponseStarted:
    """The final response has started, and its body waits until NativeRequest.read() asks for it.

    Attributes:
        status: The HTTP status code.
        protocol: The protocol the response arrived over, for example "http/1.1" or "h2".
        url: The final URL, after redirects.
        headers: The (name, value) pairs in the order received, so repeated headers stay separate. Headers have no fixed
            encoding, so they are decoded as Latin-1, which keeps every byte: value.encode("latin-1") gives back what the
            server sent.
    """

    status: int
    protocol: str
    url: str
    headers: tuple[tuple[str, str], ...]


@dataclass(frozen=True, slots=True)
class RedirectFollowed:
    """A redirect arrived and //net follows it.

    Attributes:
        status: The HTTP status code of the redirect.
        url: The URL that it leads to.
        headers: The (name, value) pairs of the redirect, as in ResponseStarted.
    """

    status: int
    url: str
    headers: tuple[tuple[str, str], ...]


@dataclass(frozen=True, slots=True)
class BodyAvailable:
    """Body bytes that NativeRequest.read() asked for have arrived."""


@dataclass(frozen=True, slots=True)
class Finished:
    """The request has ended, and no event comes after this one.

    Attributes:
        net_error: 0 if the whole body was read, otherwise a negative Chromium net error code. An HTTP error status such
            as 404 is not a net error. -3 (ERR_ABORTED) says that the request was cancelled.
    """

    net_error: int


NativeEvent = ResponseStarted | RedirectFollowed | BodyAvailable | Finished


@dataclass(frozen=True, slots=True)
class NativeResponse:
    """A copy of a finished response from the native core.

    Attributes:
        status: The HTTP status code of the final response, after redirects.
        protocol: The protocol the response arrived over, for example "http/1.1" or "h2".
        url: The final URL, after redirects.
        headers: The final response's (name, value) pairs in the order received, so repeated headers stay separate.
            Headers have no fixed encoding, so they are decoded as Latin-1, which keeps every byte:
            value.encode("latin-1") gives back what the server sent.
        body: The final response's body, after any content decoding. It stays out of repr(), since it can be large.
    """

    status: int
    protocol: str
    url: str
    headers: tuple[tuple[str, str], ...]
    body: bytes = field(repr=False)


class NativeRequest:
    """A request that a Session has started, driven by its caller. Release it with close() or a with statement.

    Each call into the native core releases the GIL, and nothing in the native core ever waits for it.

    Attributes:
        identity: What Session.next_ready_request() names this request by.
    """

    def __init__(self, library: ctypes.CDLL, handle: Any, url: str, method: str) -> None:
        self._library = library
        self._handle = handle
        self._url = url
        self._method = method
        self.identity: int = ctypes.addressof(handle.contents)

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    def next_event(self, timeout: float | None = None) -> NativeEvent | None:
        """Take the next event, which waits in the native core for as long as the caller takes.

        Args:
            timeout: How many seconds to wait for an event, or None to wait for as long as it takes.

        Returns:
            The event, or None if none arrived in time.

        Raises:
            RequestError: If the Finished event has been taken already, or this request has been closed.
        """
        timeout_ms = -1 if timeout is None else math.ceil(timeout * 1000)
        native = MittoEvent()
        result: int = self._library.mitto_request_next_event(self._handle, timeout_ms, ctypes.byref(native))
        if result == _ERR_IO_PENDING:
            return None
        if result != 0:
            raise self.error(result)
        try:
            return _copy_event(native)
        finally:
            self._library.mitto_event_free(ctypes.byref(native))

    def read(self, size: int = _READ_SIZE) -> bytes | None:
        """Read body bytes that have arrived, without waiting.

        The native core reads the body from the network only when this asks for it, so a caller who does not read makes
        the server wait through flow control.

        Args:
            size: The most bytes to return.

        Returns:
            The bytes. None if none are waiting: this asks for more, and a BodyAvailable event says when to call it again,
            or a Finished event that no more will come. An empty value once the request has finished and the bytes that
            arrived before that have been read.

        Raises:
            RequestError: If `size` is not positive, or this request has been closed.
        """
        buffer = ctypes.create_string_buffer(max(size, 0))
        result: int = self._library.mitto_request_read(self._handle, buffer, len(buffer))
        if result == _ERR_IO_PENDING:
            return None
        if result < 0:
            raise self.error(result)
        return ctypes.string_at(buffer, result)

    def error(self, net_error: int) -> RequestError:
        """Return the error that a Finished event's net error stands for."""
        return _request_error(self._library, self._url, net_error, self._method)

    def cancel(self) -> None:
        """End the request, without waiting for that. Unless it finishes first, its Finished event holds ERR_ABORTED."""
        self._library.mitto_request_cancel(self._handle)

    def close(self) -> None:
        """Cancel the request if it is still running, and release it. Later calls do nothing or raise a RequestError."""
        self._library.mitto_request_release(self._handle)
        self._handle = ctypes.POINTER(MittoRequest)()


class PrivateSettings(TypedDict, total=False):
    """The Session settings that the public API does not offer, for tests to pass to the public Session as `_settings`.

    The keys are arguments of Session.__init__ here, which says what they mean.
    """

    ignore_certificate_errors_spki_list: Sequence[str]
    origins_to_force_quic_on: Sequence[str]
    dns_nameserver: str | None
    host_resolver_rules: str


class Session:
    """A Session in the native core, with its own request context.

    One Session may run requests from several threads at once. Closing it ends the ones still running, but it must not
    be closed while another thread is starting one.
    """

    def __init__(
        self,
        *,
        ignore_certificate_errors_spki_list: Sequence[str] = (),
        additional_trust_anchors: Sequence[str] = (),
        accept_language: str = "",
        origins_to_force_quic_on: Sequence[str] = (),
        dns_nameserver: str | None = None,
        host_resolver_rules: str = "",
    ) -> None:
        """Create the Session.

        Args:
            ignore_certificate_errors_spki_list: Accept any certificate chain that holds one of these keys, with the
                meaning of Chrome's --ignore-certificate-errors-spki-list. Each entry is the Base64 form of the SHA-256
                hash of a DER-encoded SubjectPublicKeyInfo.
            additional_trust_anchors: Trust these certificates as roots, beside the Chrome Root Store. Each entry is the
                text of a PEM file that holds at least one.
            accept_language: The value of the Accept-Language header that //net adds to each request whose headers do not
                hold one. An empty string adds no header.
            origins_to_force_quic_on: Reach these HTTPS origins, each written "host:port", over QUIC and nothing else,
                with the meaning of Chrome's --origin-to-force-quic-on. It also lifts //net's refusal of QUIC to a
                certificate from an unknown root, for each of their hosts. The certificate still has to verify. "*"
                stands for every origin. Without any, a request learns of QUIC from Alt-Svc.
            dns_nameserver: Ask only this DNS server, written "address:port" with an IPv6 address in brackets, in place of
                what the system's settings name. It is for tests, so the public API does not offer it.
            host_resolver_rules: Rules that change which address a name resolves to, separated by commas, with the meaning
                of Chrome's --host-resolver-rules. Only the name's resolution changes, so SNI, the certificate check and
                the Host header keep the name.

        Raises:
            ValueError: If the native core refuses the settings.
            NativeLibraryError: If the native library cannot be used.
        """
        self._library = load_library()
        settings_dict: dict[str, object] = {
            "version": SETTINGS_VERSION,
            "ignore_certificate_errors_spki_list": list(ignore_certificate_errors_spki_list),
            "additional_trust_anchors": list(additional_trust_anchors),
            "accept_language": accept_language,
            "origins_to_force_quic_on": list(origins_to_force_quic_on),
            "host_resolver_rules": host_resolver_rules,
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
        """Close the Session's connections and release it, which ends each request still running with ERR_ABORTED.

        A NativeRequest stays usable until it is closed. Later requests fail with a RequestError.
        """
        self._library.mitto_session_destroy(self._handle)
        self._handle = ctypes.POINTER(MittoSession)()

    def event_fd(self) -> int:
        """Return a file descriptor that is readable while any of this Session's requests has an event waiting.

        This is for a caller that cannot block, such as an event loop. Get it before starting requests, because only
        events that arrive after the first call count. The Session owns it, so it must not be closed or read directly.

        Raises:
            OSError: If there is no room for another file descriptor.
        """
        fd = ctypes.c_int32()
        net_error: int = self._library.mitto_session_event_fd(self._handle, ctypes.byref(fd))
        if net_error != 0:
            raise OSError(f"the native core has no file descriptor to give, with net error {net_error}")
        return fd.value

    def next_ready_request(self) -> int | None:
        """Take the entry for the next waiting event, which is what keeps event_fd() readable.

        Returns:
            The NativeRequest.identity of the request whose event is waiting, or None if no event is waiting. A request
            with several events waiting comes up once for each. The request may have been closed, or its identity may
            be in use by a newer one that has no event yet, so expect NativeRequest.next_event(0) to find nothing at
            times.
        """
        request = ctypes.POINTER(MittoRequest)()
        self._library.mitto_session_next_ready_request(self._handle, ctypes.byref(request))
        return ctypes.addressof(request.contents) if request else None

    def start(
        self,
        url: str,
        headers: Sequence[tuple[str, str]] = (),
        method: str = "GET",
        # The value of DEFAULT_PRIORITY in //net.
        priority: Priority = Priority.LOWEST,
        # The value of kDefaultPriorityIncremental in //net.
        priority_incremental: bool = True,
        referrer: str | None = None,
        timeout: Timeout = NO_TIMEOUT,
    ) -> NativeRequest:
        """Start a request for `url` and return without waiting for it.

        Args:
            url: The URL to fetch.
            headers: The (name, value) pairs to send, in the order they are to reach the wire, encoded as Latin-1. A
                name may appear only once, ignoring case. //net adds its own headers as it does in Chrome, such as Host
                before these and Accept-Encoding after them, unless they are among these. Referer cannot be among these,
                because //net only sends `referrer`.
            method: The HTTP method, sent as it is written here. Chrome never sends CONNECT, TRACE or TRACK, so they are
                refused.
            priority: The priority of the request.
            priority_incremental: Whether the response can be used as it arrives. With `priority`, this decides the
                HTTP/2 weight, the priority header and the HTTP/3 PRIORITY_UPDATE.
            referrer: The URL to send as the Referer header, or None to send none. //net puts it after `headers` and
                before the headers it adds, and leaves out any username, password and fragment.
            timeout: The timers of the request. One that runs out arrives as the Finished event.

        Raises:
            ValueError: If `url` or `referrer` holds a NUL character or cannot be encoded as UTF-8, or a header cannot be
                encoded as Latin-1.
            RequestError: If the native core refuses the request with ERR_INVALID_ARGUMENT, because of an invalid header or
                method, or a `referrer` that //net cannot send. What goes wrong later arrives as the Finished event.
        """
        encoded_url = encode_url(url)
        options: dict[str, object] = {
            "version": REQUEST_OPTIONS_VERSION,
            "method": method,
            "priority": int(priority),
            "priority_incremental": priority_incremental,
            **timeout_options(timeout),
        }
        if referrer is not None:
            encode_url(referrer)
            options["referrer"] = referrer
        encoded_options = json.dumps(options).encode()
        # This list keeps the bytes alive during the call, since the structs only point to them.
        encoded_headers = [(name.encode("latin-1"), value.encode("latin-1")) for name, value in headers]
        native_headers = (MittoHeader * len(encoded_headers))(
            *(MittoHeader(name, len(name), value, len(value)) for name, value in encoded_headers)
        )

        handle = ctypes.POINTER(MittoRequest)()
        net_error: int = self._library.mitto_request_start(
            self._handle,
            encoded_url,
            native_headers,
            len(native_headers),
            encoded_options,
            len(encoded_options),
            ctypes.byref(handle),
        )
        if net_error != 0:
            raise _request_error(self._library, url, net_error, method)
        return NativeRequest(self._library, handle, url, method)

    def request(
        self,
        url: str,
        headers: Sequence[tuple[str, str]] = (),
        method: str = "GET",
        # The value of DEFAULT_PRIORITY in //net.
        priority: Priority = Priority.LOWEST,
        # The value of kDefaultPriorityIncremental in //net.
        priority_incremental: bool = True,
        referrer: str | None = None,
        timeout: Timeout = NO_TIMEOUT,
    ) -> NativeResponse:
        """Fetch `url` and block until the whole response has arrived, with the GIL released while it waits.

        The arguments are those of start().

        Raises:
            ValueError: If `url` or `referrer` holds a NUL character or cannot be encoded as UTF-8, or a header cannot be
                encoded as Latin-1.
            RequestError: If the request ends without a response, of the subclass that suits its net error. An HTTP error
                status such as 404 is a response. An invalid header or method, or a `referrer` that //net cannot send,
                ends the request with ERR_INVALID_ARGUMENT.
        """
        with self.start(url, headers, method, priority, priority_incremental, referrer, timeout) as request:
            return _collect_response(request)


def _collect_response(request: NativeRequest) -> NativeResponse:
    """Drive `request` to its end and return the whole response.

    Raises:
        RequestError: If the request ends without a response.
    """
    started: ResponseStarted | None = None
    chunks: list[bytes] = []
    while not isinstance(event := request.next_event(), Finished):
        if isinstance(event, ResponseStarted):
            started = event
        if isinstance(event, ResponseStarted | BodyAvailable):
            # This stops at the point where a read asks for more, which leads to the next event.
            while chunk := request.read():
                chunks.append(chunk)
    if event.net_error != 0 or started is None:
        raise request.error(event.net_error)
    return NativeResponse(
        status=started.status,
        protocol=started.protocol,
        url=started.url,
        headers=started.headers,
        body=b"".join(chunks),
    )


def _copy_event(native: MittoEvent) -> NativeEvent:
    """Return a copy of an event from the native core, which owns its buffers."""
    match EventType(native.type):
        case EventType.RESPONSE_STARTED:
            return ResponseStarted(
                status=native.status,
                protocol=native.protocol.decode(),
                url=native.url.decode(),
                headers=_parse_headers(ctypes.string_at(native.headers, native.headers_size)),
            )
        case EventType.REDIRECT_FOLLOWED:
            return RedirectFollowed(
                status=native.status,
                url=native.url.decode(),
                headers=_parse_headers(ctypes.string_at(native.headers, native.headers_size)),
            )
        case EventType.BODY_AVAILABLE:
            return BodyAvailable()
        case EventType.FINISHED:
            return Finished(net_error=native.net_error)


def _parse_headers(block: bytes) -> tuple[tuple[str, str], ...]:
    """Split the C API's "name: value\\r\\n" lines into pairs.

    Only CRLF ends a line. str.splitlines() would also split on characters that a value may hold, such as 0x85.
    """
    lines = block.decode("latin-1").split("\r\n")
    return tuple((name, value) for name, _, value in (line.partition(": ") for line in lines if line))
