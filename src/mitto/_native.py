"""The private binding of the native core's C API.

Nothing is loaded until load_library() is called, so the package can be imported on a machine without the native
library. The public Session is built on the Session here, and the settings that only tests need stay private.
"""

import ctypes
import enum
import errno
import functools
import json
import math
import os
import sys
from collections.abc import Buffer, Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field, fields
from pathlib import Path
from types import TracebackType
from typing import Any, Self, TypedDict

from mitto._body import StreamBody
from mitto._errors import NativeLibraryError, RequestError, request_error
from mitto._timeout import NO_TIMEOUT, Timeout

__all__ = [
    "ABI_VERSION",
    "COOKIES_VERSION",
    "LIBRARY_ENV_VAR",
    "LIBRARY_NAME",
    "MEMORY_CACHE",
    "PAGE_OPTIONS_VERSION",
    "REQUEST_OPTIONS_VERSION",
    "SETTINGS_VERSION",
    "WRITE_SIZE",
    "BodyAvailable",
    "BodyWanted",
    "BrandVersion",
    "ConsoleLevel",
    "ConsoleMessage",
    "DeprecatedUnload",
    "EventType",
    "FetchParams",
    "Finished",
    "MittoEvent",
    "MittoHeader",
    "MittoPage",
    "MittoPageEvent",
    "MittoRequest",
    "MittoSession",
    "NativeEvent",
    "NativeLibraryError",
    "NativePage",
    "NativePageEvent",
    "NativeRequest",
    "NativeResponse",
    "NetworkQuality",
    "PageEnded",
    "PageErrorCode",
    "PageEvaluated",
    "PageEventType",
    "PageFeatures",
    "PageLoaded",
    "PageMediaDevices",
    "PageNavigated",
    "PageNavigator",
    "PageRequestTemplate",
    "PageStart",
    "PageUserAgentData",
    "Priority",
    "PrivateSettings",
    "ReadyKind",
    "RedirectFollowed",
    "RequestError",
    "ResponseStarted",
    "Session",
    "UncaughtError",
    "UnloadBuckets",
    "encode_url",
    "find_library",
    "finished_response",
    "iter_body",
    "load_library",
    "timeout_options",
    "wait_for_response",
]

# The MITTO_ABI_VERSION of the C header this module is written against. Change it together with the declarations below.
ABI_VERSION = 11

# The MITTO_SETTINGS_VERSION of the C header this module is written against.
SETTINGS_VERSION = 1

# The MITTO_REQUEST_OPTIONS_VERSION of the C header this module is written against.
REQUEST_OPTIONS_VERSION = 1

# The MITTO_COOKIES_VERSION of the C header this module is written against.
COOKIES_VERSION = 1

# The MITTO_PAGE_OPTIONS_VERSION of the C header this module is written against.
PAGE_OPTIONS_VERSION = 4

# Names the library of a local build. It takes precedence over the library in the installed package.
LIBRARY_ENV_VAR = "MITTO_NATIVE_LIBRARY"

# The file name of the library in the installed package, for each Host.
LIBRARY_NAME = {"darwin": "libmitto_core.dylib", "win32": "mitto_core.dll"}.get(sys.platform, "libmitto_core.so")

# What the C API returns when there is nothing yet.
_ERR_IO_PENDING = -1

# What mitto_session_create() returns for settings that it refuses, and when another Session has a directory for a cache.
_ERR_INVALID_ARGUMENT = -4
_ERR_CACHE_LOCK_TIMEOUT = -409

# What mitto_page_open() returns when no Page program lies beside the library, and mitto_page_evaluate() for too long a
# script.
_ERR_FILE_NOT_FOUND = -6
_ERR_MSG_TOO_BIG = -142

# The longest script, in bytes of UTF-8, that a Page takes: what one message over its channel carries.
MAX_SCRIPT_SIZE = 16 * 1024 * 1024 - 8

# The file name of the Page program, which a build with the Runtime puts beside the library.
PAGE_PROGRAM_NAME = "mitto_page.exe" if sys.platform == "win32" else "mitto_page"

# What the "http_cache" setting says for a cache that is not in a directory.
MEMORY_CACHE = "memory"

# How much a read asks for: as much as the native core holds at a time.
_READ_SIZE = 64 * 1024

# How much a write gives: as much as the native core holds at a time.
WRITE_SIZE = 64 * 1024

_PROXY_SCHEMES = frozenset({"http", "https", "socks5"})


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


class MittoPage(ctypes.Structure):
    """The opaque type behind a Page handle in the C API."""


class ReadyKind(enum.IntEnum):
    """What Session.next_ready() names, with the values of the C header's MITTO_READY constants."""

    REQUEST = 1
    PAGE = 2


class PageEventType(enum.IntEnum):
    """The type of a Page's event, with the values of the C header's MITTO_PAGE_EVENT constants."""

    EVALUATED = 1
    ENDED = 2
    CONSOLE = 3
    UNCAUGHT_ERROR = 4
    NAVIGATED = 5
    LOADED = 6
    KNOWN_GAP = 7


class ConsoleLevel(enum.IntEnum):
    """The level of a console message, with the values of the C header's MITTO_PAGE_CONSOLE constants."""

    DEBUG = 1
    LOG = 2
    INFO = 3
    WARNING = 4
    ERROR = 5


class PageErrorCode(enum.IntEnum):
    """Why a call to a Page failed or the Page ended, with the values of the C header's MITTO_PAGE_ERROR constants."""

    NONE = 0
    SCRIPT = 1
    PROCESS_ENDED = 2
    REFUSED = 3
    SESSION_DESTROYED = 4


class EventType(enum.IntEnum):
    """The type of a request's event, with the values of the C header's MITTO_EVENT constants."""

    RESPONSE_STARTED = 1
    REDIRECT_FOLLOWED = 2
    BODY_AVAILABLE = 3
    FINISHED = 4
    BODY_WANTED = 5


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
        ("from_cache", ctypes.c_int32),
        ("protocol", ctypes.c_char_p),
        ("url", ctypes.c_char_p),
        ("headers", ctypes.POINTER(ctypes.c_char)),
        ("headers_size", ctypes.c_size_t),
    )


class MittoPageEvent(ctypes.Structure):
    """One event of a Page, laid out like the C struct of the same name.

    The native core owns the buffers. Copy what is needed, then release them with mitto_page_event_free().
    """

    _fields_ = (
        ("type", ctypes.c_int32),
        ("error", ctypes.c_int32),
        ("call", ctypes.c_uint64),
        ("json", ctypes.POINTER(ctypes.c_char)),
        ("json_size", ctypes.c_size_t),
        ("message", ctypes.POINTER(ctypes.c_char)),
        ("message_size", ctypes.c_size_t),
        ("level", ctypes.c_int32),
        ("status", ctypes.c_int32),
        ("net_error", ctypes.c_int32),
        ("url", ctypes.POINTER(ctypes.c_char)),
        ("url_size", ctypes.c_size_t),
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
        library.mitto_session_list_cookies.argtypes = (
            session_pointer,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_char)),
            ctypes.POINTER(ctypes.c_size_t),
        )
        library.mitto_session_list_cookies.restype = ctypes.c_int32
        library.mitto_session_set_cookies.argtypes = (session_pointer, ctypes.c_char_p, ctypes.c_size_t)
        library.mitto_session_set_cookies.restype = ctypes.c_int32
        library.mitto_session_delete_cookies.argtypes = (
            session_pointer,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint32),
        )
        library.mitto_session_delete_cookies.restype = ctypes.c_int32
        library.mitto_cookies_free.argtypes = (ctypes.POINTER(ctypes.c_char),)
        library.mitto_cookies_free.restype = None
        library.mitto_session_event_fd.argtypes = (session_pointer, ctypes.POINTER(ctypes.c_int32))
        library.mitto_session_event_fd.restype = ctypes.c_int32
        library.mitto_session_next_ready.argtypes = (
            session_pointer,
            ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_void_p),
        )
        library.mitto_session_next_ready.restype = ctypes.c_int32
        library.mitto_session_wait_for_event.argtypes = (session_pointer, ctypes.c_int64)
        library.mitto_session_wait_for_event.restype = ctypes.c_int32
        library.mitto_session_end_waits.argtypes = (session_pointer,)
        library.mitto_session_end_waits.restype = None
        library.mitto_request_start.argtypes = (
            session_pointer,
            ctypes.c_char_p,
            ctypes.POINTER(MittoHeader),
            ctypes.c_size_t,
            ctypes.c_char_p,
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
        library.mitto_request_write_body.argtypes = (request_pointer, ctypes.c_char_p, ctypes.c_size_t)
        library.mitto_request_write_body.restype = ctypes.c_int32
        library.mitto_request_end_body.argtypes = (request_pointer,)
        library.mitto_request_end_body.restype = ctypes.c_int32
        library.mitto_request_cancel.argtypes = (request_pointer,)
        library.mitto_request_cancel.restype = None
        library.mitto_request_release.argtypes = (request_pointer,)
        library.mitto_request_release.restype = None
        library.mitto_event_free.argtypes = (ctypes.POINTER(MittoEvent),)
        library.mitto_event_free.restype = None
        page_pointer = ctypes.POINTER(MittoPage)
        library.mitto_page_open.argtypes = (
            session_pointer,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(page_pointer),
        )
        library.mitto_page_open.restype = ctypes.c_int32
        library.mitto_page_process_id.argtypes = (page_pointer, ctypes.POINTER(ctypes.c_int64))
        library.mitto_page_process_id.restype = ctypes.c_int32
        library.mitto_page_evaluate.argtypes = (
            page_pointer,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint64),
        )
        library.mitto_page_evaluate.restype = ctypes.c_int32
        library.mitto_page_next_event.argtypes = (page_pointer, ctypes.c_int64, ctypes.POINTER(MittoPageEvent))
        library.mitto_page_next_event.restype = ctypes.c_int32
        library.mitto_page_event_free.argtypes = (ctypes.POINTER(MittoPageEvent),)
        library.mitto_page_event_free.restype = None
        library.mitto_page_end_waits.argtypes = (page_pointer,)
        library.mitto_page_end_waits.restype = None
        library.mitto_page_close.argtypes = (page_pointer,)
        library.mitto_page_close.restype = None
    except (AttributeError, OSError) as error:
        raise NativeLibraryError(f"cannot use {path} as the native library: {error}") from error

    return library


def encode_url(url: str, argument: str = "url") -> bytes:
    """Return `url`, which a caller gave as `argument`, the way the C API takes it.

    Raises:
        ValueError: If `url` holds a NUL character, or a lone surrogate, which UTF-8 cannot encode. The message names
            `argument` and leaves out the URL, which may hold a secret.
    """
    try:
        encoded_url = url.encode()
    except UnicodeEncodeError:
        # Its own message shows the character, which is part of the URL.
        raise ValueError(f"{argument} holds a lone surrogate, which UTF-8 cannot encode") from None
    # The C API takes a NUL-terminated string, so it would fetch only what comes before the NUL.
    if b"\0" in encoded_url:
        raise ValueError(f"{argument} holds a NUL character")
    return encoded_url


def _check_proxy(proxy: str) -> None:
    """Refuse a proxy whose fault a message can name, which the native core refuses too with only a net error.

    A proxy's URL may hold a password, and until it parses nothing says which part that is, so the message repeats none of
    it.

    Raises:
        ValueError: If `proxy` has no scheme that names a kind of proxy.
    """
    scheme, separator, _ = proxy.partition("://")
    if not separator or scheme.lower() not in _PROXY_SCHEMES:
        raise ValueError("a proxy is a URL whose scheme is http, https or socks5")


def _cache_error(library: ctypes.CDLL, http_cache: str | None, net_error: int) -> OSError:
    """Return the error for a net error from mitto_session_create() that is not about the form of its settings."""
    if net_error == _ERR_CACHE_LOCK_TIMEOUT:
        return OSError(errno.EBUSY, "another Session has this cache open", http_cache)
    name: bytes = library.mitto_net_error_name(net_error)
    return OSError(f"cannot keep a cache in {http_cache}: {name.decode()}")


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
        from_cache: Whether the body comes from the Session's HTTP cache, also when the server has answered a revalidation
            with a 304.
    """

    status: int
    protocol: str
    url: str
    headers: tuple[tuple[str, str], ...]
    from_cache: bool = False


@dataclass(frozen=True, slots=True)
class RedirectFollowed:
    """A redirect arrived and //net follows it.

    The next event's URL says where it led, unless the request ends first.

    Attributes:
        status: The HTTP status code of the redirect.
        url: The URL that answered with the redirect.
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


@dataclass(frozen=True, slots=True)
class BodyWanted:
    """//net has taken all of a stream body so far, and NativeRequest.send_body() gives it more."""


NativeEvent = ResponseStarted | RedirectFollowed | BodyAvailable | Finished | BodyWanted


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
        history: The redirects that led here, the first one first.
        from_cache: Whether the body comes from the Session's HTTP cache, also when the server has answered a revalidation
            with a 304.
    """

    status: int
    protocol: str
    url: str
    headers: tuple[tuple[str, str], ...]
    body: bytes = field(repr=False)
    history: tuple[RedirectFollowed, ...] = ()
    from_cache: bool = False


class NativeRequest:
    """A request that a Session has started, driven by its caller. Release it with close() or a with statement.

    Each call into the native core releases the GIL, and nothing in the native core ever waits for it.

    Attributes:
        identity: What Session.next_ready() names this request by.
    """

    def __init__(
        self, library: ctypes.CDLL, handle: Any, url: str, method: str, body_chunks: Iterator[memoryview] | None = None
    ) -> None:
        self._library = library
        self._handle = handle
        self._url = url
        self._method = method
        self._body_chunks = body_chunks
        self._unsent_body = memoryview(b"")
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

    def write_body(self, data: Buffer) -> int | None:
        """Give the native core bytes of a stream body, without waiting.

        Args:
            data: The bytes, of which it takes as much as it holds.

        Returns:
            How many it took. 0 if nothing more of the body is wanted, because the request has finished or a 303 has dropped
            the body. None if it has no room, and a BodyWanted event says when to call this again.

        Raises:
            RequestError: If `data` is empty, the request has no stream, the body has ended, `data` goes past the size of the
                stream or this request has been closed.
        """
        chunk = bytes(data)
        result: int = self._library.mitto_request_write_body(self._handle, chunk, len(chunk))
        if result == _ERR_IO_PENDING:
            return None
        if result < 0:
            raise self.error(result)
        return result

    def end_body(self) -> None:
        """Say that nothing more of a stream body will come, without waiting.

        Raises:
            RequestError: If the request has no stream, the body has ended already or this request has been closed.
        """
        result: int = self._library.mitto_request_end_body(self._handle)
        if result != 0:
            raise self.error(result)

    def send_body(self) -> None:
        """Answer a BodyWanted event from the stream body given to Session.start().

        This waits for nothing but the caller's file or iterable, so that whatever waits for events can decide where that
        wait belongs. Any part too large for the native core waits for the next event.

        Raises:
            RequestError: If this request has been closed.
            Exception: Whatever the caller's file or iterable raises, after which the request should be closed, so that
                what has gone does not count as the whole body.
        """
        if self._body_chunks is None:
            return
        while not self._unsent_body.nbytes:
            chunk = next(self._body_chunks, None)
            if chunk is None:
                self._body_chunks = None
                self.end_body()
                return
            self._unsent_body = chunk
        written = self.write_body(self._unsent_body[:WRITE_SIZE])
        if written == 0:
            self._body_chunks = None
        elif written is not None:
            self._unsent_body = self._unsent_body[written:]

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


@dataclass(frozen=True, slots=True)
class PageEvaluated:
    """A script that NativePage.evaluate() sent has its result, or its call has failed.

    Attributes:
        call: The number of the call, as NativePage.evaluate() returned it.
        error: PageErrorCode.NONE if the script has a result, and otherwise why the call failed.
        result: The result as JSON, as JSON.stringify() writes it. Empty if it writes nothing, as for undefined, and if the
            call failed. The Page process writes it, so a caller reads it as it would read JSON from a server.
        message: Why the call failed, such as "Uncaught TypeError: x is not a function", or empty.
    """

    call: int
    error: PageErrorCode
    result: str = ""
    message: str = ""


@dataclass(frozen=True, slots=True)
class PageEnded:
    """The Page has ended, and each later call fails with the same error.

    Attributes:
        error: Why it ended.
        message: How it ended, such as "the Page process was ended by signal 9".
    """

    error: PageErrorCode
    message: str


@dataclass(frozen=True, slots=True)
class ConsoleMessage:
    """A message that a Page's script wrote to the console.

    Attributes:
        level: "debug", "log", "info", "warning" or "error", as the console method that wrote it says: console.warn()
            writes "warning", and V8 sorts the other methods among these.
        text: The message, with its arguments written as V8's inspector writes them for Chrome's log, separated by spaces.
    """

    level: str
    text: str


@dataclass(frozen=True, slots=True)
class UncaughtError:
    """An error that no script of the Page caught, such as a promise rejected with no handler. The Page goes on.

    Attributes:
        text: What a console says of it, such as "Uncaught (in promise) Error: no".
    """

    text: str


@dataclass(frozen=True, slots=True)
class KnownGap:
    """The document reached a member whose behaviour the Runtime lacks, for the first time. The Page goes on.

    Attributes:
        name: The member, such as "URL.prototype.href" or "new Highlight".
    """

    name: str


@dataclass(frozen=True, slots=True)
class PageNavigated:
    """The navigation that opening the Page started has its response, and its document takes about:blank's place; or it
    failed before a response, and the Page keeps about:blank.

    Attributes:
        url: The URL of the document, after any redirects, or the URL that the navigation failed at.
        status: The response's HTTP status code, or 0 if it failed.
        net_error: 0 if the navigation has a response, and otherwise the negative Chromium net error code it failed with,
            which NativePage.request_error() turns into the error that a request raises for it.
    """

    url: str
    status: int
    net_error: int


@dataclass(frozen=True, slots=True)
class PageLoaded:
    """The window's load event has been dispatched: the document and its classic scripts have loaded and run."""


NativePageEvent = PageEvaluated | PageEnded | ConsoleMessage | UncaughtError | KnownGap | PageNavigated | PageLoaded


class NativePage:
    """A Page that a Session has opened, whose scripts run in a process of its own. Release it with close().

    Each call into the native core releases the GIL, and nothing in the native core ever waits for it. No call may use it
    once close() has begun, so a thread that waits in next_event() ends its wait with end_waits() first.

    Attributes:
        identity: What Session.next_ready() names this Page by.
        process_id: The ID of the Page's process, which stays the same after the process has ended.
    """

    def __init__(self, library: ctypes.CDLL, handle: Any) -> None:
        self._library = library
        self._handle = handle
        self.identity: int = ctypes.addressof(handle.contents)
        process_id = ctypes.c_int64()
        library.mitto_page_process_id(handle, ctypes.byref(process_id))
        self.process_id: int = process_id.value

    def evaluate(self, script: str) -> int:
        """Send `script` to the Page, which runs it as a classic script after the ones sent before it, without waiting.

        Returns:
            The number of the call, which the PageEvaluated event of its result holds.

        Raises:
            ValueError: If `script` holds a lone surrogate, which UTF-8 cannot encode, or is longer than MAX_SCRIPT_SIZE
                bytes of UTF-8, or this Page has been closed.
        """
        try:
            encoded = script.encode()
        except UnicodeEncodeError:
            # Its own message shows the character, which is part of the script.
            raise ValueError("the script holds a lone surrogate, which UTF-8 cannot encode") from None
        call = ctypes.c_uint64()
        result: int = self._library.mitto_page_evaluate(self._handle, encoded, len(encoded), ctypes.byref(call))
        if result == _ERR_MSG_TOO_BIG:
            raise ValueError(f"the script is {len(encoded)} bytes of UTF-8, and a Page takes at most {MAX_SCRIPT_SIZE}")
        if result != 0:
            raise ValueError("this Page has been closed")
        return call.value

    def next_event(self, timeout: float | None = None) -> NativePageEvent | None:
        """Take the next event, which waits in the native core for as long as the caller takes.

        Args:
            timeout: How many seconds to wait for an event, or None to wait for as long as it takes.

        Returns:
            The event, or None if none arrived in time, or end_waits() ended the wait.

        Raises:
            ValueError: If this Page has been closed.
        """
        timeout_ms = -1 if timeout is None else math.ceil(timeout * 1000)
        native = MittoPageEvent()
        result: int = self._library.mitto_page_next_event(self._handle, timeout_ms, ctypes.byref(native))
        if result == _ERR_IO_PENDING:
            return None
        if result != 0:
            raise ValueError("this Page has been closed")
        try:
            return _copy_page_event(native)
        finally:
            self._library.mitto_page_event_free(ctypes.byref(native))

    def end_waits(self) -> None:
        """End each wait in next_event() on another thread, and make each later one return at once if no event waits."""
        self._library.mitto_page_end_waits(self._handle)

    def request_error(self, url: str, net_error: int) -> RequestError:
        """Return the error that a GET request for `url` raises for `net_error`, as a navigation that failed with it."""
        return _request_error(self._library, url, net_error, "GET")

    def close(self) -> None:
        """End the Page's process, wait for it to go, which takes up to 2 seconds before a kill, and release the Page.

        Later calls do nothing or raise a ValueError.
        """
        self._library.mitto_page_close(self._handle)
        self._handle = ctypes.POINTER(MittoPage)()


@dataclass(frozen=True, slots=True)
class FetchParams:
    """What a request made for a page says of itself, in the terms of Chrome's network::ResourceRequest.

    Attributes:
        mode: "navigate", "cors" or "no-cors".
        destination: What Sec-Fetch-Dest says, such as "document" or "empty".
        credentials_mode: "same-origin", as a page's fetch() has unless it asks otherwise, or "include".
        has_user_activation: Whether Sec-Fetch-User says that the user started it.
        initiator: The URL of the page it comes from. Without one, a navigation counts as started by the browser and a
            subresource as coming from the page at the root of the origin of its URL.
        fixed_headers: The names of the headers that are the caller's, in any case, which keep what they say on each hop.
            A name that the headers do not hold stays out of them.
    """

    mode: str
    destination: str
    credentials_mode: str
    has_user_activation: bool = False
    initiator: str | None = None
    fixed_headers: tuple[str, ...] = ()

    def to_option(self) -> dict[str, object]:
        """Return the "fetch" option of mitto_request_start().

        Raises:
            ValueError: If `initiator` holds a NUL character or cannot be encoded as UTF-8.
        """
        option: dict[str, object] = {
            "mode": self.mode,
            "destination": self.destination,
            "credentials_mode": self.credentials_mode,
            "has_user_activation": self.has_user_activation,
            "fixed_headers": list(self.fixed_headers),
        }
        if self.initiator is not None:
            encode_url(self.initiator, "initiator")
            option["initiator"] = self.initiator
        return option


@dataclass(frozen=True, slots=True)
class BrandVersion:
    """A brand of a Chrome Release and its version, as sec-ch-ua lists them.

    Attributes:
        brand: The brand, such as "Google Chrome".
        version: Its version, such as "157" in sec-ch-ua or "157.0.8103.39" in sec-ch-ua-full-version-list.
    """

    brand: str
    version: str


@dataclass(frozen=True, slots=True, kw_only=True)
class PageUserAgentData:
    """What Chrome's browser tells each renderer of its user agent, which navigator.userAgentData gives a page: Blink's
    UserAgentMetadata, which the user agent client hints also come from.

    Attributes:
        brands: The brands with their major versions, in sec-ch-ua's order.
        full_version_list: The brands with their full versions, in sec-ch-ua-full-version-list's order.
        mobile: Whether the browser claims a phone, as sec-ch-ua-mobile says.
        platform: The operating system, such as "Windows", as sec-ch-ua-platform says.
        platform_version: Its version, as sec-ch-ua-platform-version says, such as "19.0.0".
        architecture: The processor's architecture, such as "x86".
        bitness: "64" or "32".
        model: The device's model, "" on a desktop.
        ua_full_version: The full version of the Chrome Release, as sec-ch-ua-full-version says.
        wow64: Whether a 32-bit Chrome runs on 64-bit Windows.
        form_factors: The form factors, such as ("Desktop",).
    """

    brands: tuple[BrandVersion, ...]
    full_version_list: tuple[BrandVersion, ...]
    mobile: bool
    platform: str
    platform_version: str
    architecture: str
    bitness: str
    model: str
    ua_full_version: str
    wow64: bool
    form_factors: tuple[str, ...]

    def to_option(self) -> dict[str, object]:
        """Return the "user_agent_data" of the "navigator" of the "start" option of mitto_page_open()."""
        option: dict[str, object] = {name.name: getattr(self, name.name) for name in fields(self)}
        option["brands"] = [{"brand": item.brand, "version": item.version} for item in self.brands]
        option["full_version_list"] = [
            {"brand": item.brand, "version": item.version} for item in self.full_version_list
        ]
        option["form_factors"] = list(self.form_factors)
        return option


@dataclass(frozen=True, slots=True, kw_only=True)
class PageNavigator:
    """What Chrome's browser tells each renderer of itself, which a page's navigator reads.

    Attributes:
        user_agent: The User-Agent that the Session sends.
        platform: What navigator.platform gives, such as "Win32".
        languages: The languages of Chrome's settings, separated by commas, such as "en-US,en", from which the Session's
            Accept-Language comes.
        user_agent_data: What navigator.userAgentData gives.
    """

    user_agent: str
    platform: str
    languages: str
    user_agent_data: PageUserAgentData

    def to_option(self) -> dict[str, object]:
        """Return the "navigator" of the "start" option of mitto_page_open()."""
        return {
            "user_agent": self.user_agent,
            "platform": self.platform,
            "languages": self.languages,
            "user_agent_data": self.user_agent_data.to_option(),
        }


@dataclass(frozen=True, slots=True)
class UnloadBuckets:
    """kDeprecateUnloadByBucket's rollout, as the variations seed of a Persona's Chrome Release gives it.

    Attributes:
        percent: kDeprecateUnload's "rollout_percent": on what share of hosts, from 0 to 100, each installation has
            unload deprecated.
        bucket: "rollout_bucket", which hosts those are, or None where the seed gives each installation a bucket of
            its own. Only a percent between 0 and 100 needs it, and without it a Page deprecates unload on no host
            that the share decides.
    """

    percent: int = 100
    bucket: int | None = None


@dataclass(frozen=True, slots=True)
class DeprecatedUnload:
    """What the variations seed of a Persona's Chrome Release says of kDeprecateUnload, which takes "unload" out of the
    permissions policy of the pages it covers: the "deprecated_unload" of the "features" of the "start" option of
    mitto_page_open(), and of a Persona's "page_features".

    Attributes:
        enabled: Whether kDeprecateUnload is on. Off, every page keeps unload.
        allow_list: The hosts of kDeprecateUnloadByAllowList's "allowlist" when that feature is on, where none means
            every host, or else None.
        by_bucket: kDeprecateUnloadByBucket's rollout when that feature is on, or else None.
    """

    enabled: bool = False
    allow_list: tuple[str, ...] | None = None
    by_bucket: UnloadBuckets | None = None

    @classmethod
    def from_option(cls, option: Mapping[str, Any]) -> Self:
        """Return it from what to_option() gives, as a Persona's file holds it."""
        allow_list = option["allow_list"]
        by_bucket = option["by_bucket"]
        return cls(
            enabled=bool(option["enabled"]),
            allow_list=None if allow_list is None else tuple(str(host) for host in allow_list),
            by_bucket=None if by_bucket is None else UnloadBuckets(int(by_bucket["percent"]), by_bucket["bucket"]),
        )

    def to_option(self) -> dict[str, object]:
        """Return the "deprecated_unload" of the "features" of the "start" option of mitto_page_open()."""
        return {
            "enabled": self.enabled,
            "allow_list": None if self.allow_list is None else list(self.allow_list),
            "by_bucket": None
            if self.by_bucket is None
            else {"percent": self.by_bucket.percent, "bucket": self.by_bucket.bucket},
        }


@dataclass(frozen=True, slots=True, kw_only=True)
class PageFeatures:
    """What a Page turns on and off beside the Engine Release's runtime-enabled features and V8's defaults, as its
    Persona says: the "features" of the "start" option of mitto_page_open().

    Attributes:
        enable_blink_features: Names of runtime-enabled features to turn on, as Chrome's --enable-blink-features takes.
        disable_blink_features: Names of those to turn off after, as Chrome's --disable-blink-features takes.
        v8_flags: V8's flags, such as "--no-foo", in the order to set them, after the Engine Release's.
        deprecated_unload: What the variations seed says of kDeprecateUnload.
    """

    enable_blink_features: tuple[str, ...] = ()
    disable_blink_features: tuple[str, ...] = ()
    v8_flags: tuple[str, ...] = ()
    deprecated_unload: DeprecatedUnload = field(default_factory=DeprecatedUnload)

    def to_option(self) -> dict[str, object]:
        """Return the "features" of the "start" option of mitto_page_open()."""
        return {
            "enable_blink_features": list(self.enable_blink_features),
            "disable_blink_features": list(self.disable_blink_features),
            "v8_flags": list(self.v8_flags),
            "deprecated_unload": self.deprecated_unload.to_option(),
        }


@dataclass(frozen=True, slots=True, kw_only=True)
class PageMediaDevices:
    """The kinds of media device that the computer has, which navigator.mediaDevices.enumerateDevices() lists: the
    "media_devices" of the "start" option of mitto_page_open().

    Attributes:
        audio_input: Whether it has a microphone, or another device that records sound.
        video_input: Whether it has a camera.
        audio_output: Whether it has something that plays sound.
    """

    audio_input: bool
    video_input: bool
    audio_output: bool

    def to_option(self) -> dict[str, object]:
        """Return the "media_devices" of the "start" option of mitto_page_open()."""
        return {"audio_input": self.audio_input, "video_input": self.video_input, "audio_output": self.audio_output}


@dataclass(frozen=True, slots=True, kw_only=True)
class PageStart:
    """What a Page starts with, which it would otherwise read from the machine and the browser: the "start" option of
    mitto_page_open().

    Places and sizes on the desktop are in device-independent pixels, from the display's top left corner.

    Attributes:
        time_zone: The IANA name of the time zone that the Page's Date and Intl report.
        locale: The BCP 47 tag of the locale that the Page's Intl falls back to.
        processors: The number of logical processors, from 1 to 64.
        touch_points: How many touch points the touch screen takes, from 0, for none, to 256.
        work_area_x: Where the work area, which is the display less the taskbar, starts from the left.
        work_area_y: Where the work area starts from the top.
        work_area_width: The width of the work area.
        work_area_height: The height of the work area.
        window_state: "maximised" or "normal".
        window_x: Where Chrome's window starts from the left.
        window_y: Where Chrome's window starts from the top.
        window_width: The width of Chrome's window, with its bars and borders.
        window_height: The height of Chrome's window, with its bars and borders.
        contrast_theme: Windows' contrast theme, such as "aquatic", or "none".
        inverted_colors: "inverted" with Windows' colour filter that inverts colours, or "none".
        memory: What navigator.deviceMemory gives: 2, 4, 8, 16 or 32, and from which the Page's V8 works out its heap's
            limits.
        screen_width: The width of the primary display.
        screen_height: The height of the primary display.
        scale: The display's scale, such as 1.25, which devicePixelRatio gives.
        refresh_rate: The display's refresh rate in hertz, from 1 to 1000, at which the Page begins frames and runs its
            animation frame callbacks.
        viewport_width: The width of the window's page area.
        viewport_height: The height of the window's page area.
        color_scheme: "light" or "dark".
        reduced_motion: "no-preference" or "reduce".
        reduced_transparency: "no-preference" or "reduce".
        effective_type: The effective connection type of the network, such as "4g", as the ect hint says.
        navigator: What the browser tells the Page of itself.
        features: What the Persona's Chrome Release turns on and off.
        media_devices: The kinds of media device that the computer has.
    """

    time_zone: str
    locale: str
    processors: int
    touch_points: int
    work_area_x: int
    work_area_y: int
    work_area_width: int
    work_area_height: int
    window_state: str
    window_x: int
    window_y: int
    window_width: int
    window_height: int
    contrast_theme: str
    inverted_colors: str
    memory: int
    screen_width: int
    screen_height: int
    scale: float
    refresh_rate: int
    viewport_width: int
    viewport_height: int
    color_scheme: str
    reduced_motion: str
    reduced_transparency: str
    effective_type: str
    navigator: PageNavigator
    features: PageFeatures
    media_devices: PageMediaDevices

    def to_option(self) -> dict[str, object]:
        """Return the "start" option of mitto_page_open()."""
        option = {start_field.name: getattr(self, start_field.name) for start_field in fields(self)}
        option["navigator"] = self.navigator.to_option()
        option["features"] = self.features.to_option()
        option["media_devices"] = self.media_devices.to_option()
        return option


@dataclass(frozen=True, slots=True)
class PageRequestTemplate:
    """What a Page's request of one Request Kind sends, as its Session's Persona says: one entry of the "requests" option
    of mitto_page_open(). The native core adds the URL, and the Page's document as the Initiator.

    Attributes:
        headers: The (name, value) pairs to send, in the order they reach the wire, with no caller's among them.
        priority: The priority of the request.
        priority_incremental: Whether the response can be used as it arrives.
        fetch: What the request says of itself, with no initiator and no fixed headers.
    """

    headers: tuple[tuple[str, str], ...]
    priority: Priority
    priority_incremental: bool
    fetch: FetchParams

    def to_option(self) -> dict[str, object]:
        """Return the entry of the "requests" option of mitto_page_open()."""
        return {
            "headers": [[name, value] for name, value in self.headers],
            "options": {
                "version": REQUEST_OPTIONS_VERSION,
                "method": "GET",
                "priority": int(self.priority),
                "priority_incremental": self.priority_incremental,
                "fetch": self.fetch.to_option(),
            },
        }


@dataclass(frozen=True, slots=True)
class NetworkQuality:
    """What a Session claims that Chrome's network quality estimator has, and the salts of Chrome's noise on it.

    The native core works out rtt and downlink from these on each hop of a request made for a page, for the host of that
    hop. The names are the keys of the "network_quality" setting of mitto_session_create().

    Attributes:
        http_rtt_ms: The round trip of HTTP, in milliseconds.
        downstream_throughput_kbps: The throughput, in kbps.
        browser_salt: The salt of the browser process, from 1 to 21, which a navigation's noise takes.
        renderer_salt_key: What the salt of the renderer process of each site comes from, which the noise of a script,
            image or fetch takes.
    """

    http_rtt_ms: int
    downstream_throughput_kbps: int
    browser_salt: int
    renderer_salt_key: str

    def to_setting(self) -> dict[str, object]:
        """Return the "network_quality" setting of mitto_session_create()."""
        return {
            "http_rtt_ms": self.http_rtt_ms,
            "downstream_throughput_kbps": self.downstream_throughput_kbps,
            "browser_salt": self.browser_salt,
            "renderer_salt_key": self.renderer_salt_key,
        }


class PrivateSettings(TypedDict, total=False):
    """The Session settings that the public API does not offer, for tests to pass to the public Session as `_settings`.

    The keys are arguments of Session.__init__ here, which says what they mean.
    """

    ignore_certificate_errors_spki_list: Sequence[str]
    origins_to_force_quic_on: Sequence[str]
    dns_nameserver: str | None
    host_resolver_rules: str
    accept_ch_via_alps: Mapping[str, str]
    refused_connects: int


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
        user_agent: str = "",
        origins_to_force_quic_on: Sequence[str] = (),
        dns_nameserver: str | None = None,
        host_resolver_rules: str = "",
        proxy: str | None = None,
        accept_ch_via_alps: Mapping[str, str] | None = None,
        refused_connects: int = 0,
        http_cache: str | None = None,
        network_quality: NetworkQuality | None = None,
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
            user_agent: The value of the User-Agent header that //net adds to each request whose headers do not hold one,
                and sends in what it makes by itself: a CONNECT and a fetch for a certificate. An empty string adds an empty
                header to a request, and none to a CONNECT.
            origins_to_force_quic_on: Reach these HTTPS origins, each written "host:port", over QUIC and nothing else,
                with the meaning of Chrome's --origin-to-force-quic-on. It also lifts //net's refusal of QUIC to a
                certificate from an unknown root, for each of their hosts. The certificate still has to verify. "*"
                stands for every origin. Without any, a request learns of QUIC from Alt-Svc.
            dns_nameserver: Ask only this DNS server, written "address:port" with an IPv6 address in brackets, in place of
                what the system's settings name. It is for tests, so the public API does not offer it.
            host_resolver_rules: Rules that change which address a name resolves to, separated by commas, with the meaning
                of Chrome's --host-resolver-rules. Only the name's resolution changes, so SNI, the certificate check and
                the Host header keep the name.
            proxy: The proxy for every request, as a URL with the scheme http, https or socks5. A username and password in
                it answer the proxy when it asks, and for socks5 each may be at most 255 bytes once decoded. None reaches
                each server directly, whatever the system's settings say.
            accept_ch_via_alps: What each origin counts as having said in the ACCEPT_CH frame in ALPS, by the URL of its
                origin. No server in the tests can say anything there, so this stands for what //net would read. It is for
                tests, so the public API does not offer it.
            refused_connects: Fail this many of the first connect attempts of each of this Session's TCP and UDP sockets
                with ERR_NO_BUFFER_SPACE before they reach the system, as Windows refuses one now and then. The Session tries a
                refused connect again, with a new socket, up to 2 more times: on Windows always, and on another Host only
                when this is more than 0. It is for tests, so the public API does not offer it.
            http_cache: Keep responses as Chrome's HTTP cache does: MEMORY_CACHE until this Session closes, or the absolute
                path of a directory for later Sessions too. //net only touches its subdirectory Cache_Data there. None keeps
                no responses.
            network_quality: What the rtt and downlink of a request made for a page are worked out from on each hop, with
                Chrome's noise for its host. None leaves them as the request's headers say.

        Raises:
            ValueError: If the native core refuses the settings. The message leaves out `proxy`, which may hold a password.
            OSError: If a cache cannot be kept in the directory of `http_cache`, or another Session has it open, in any
                process.
            NativeLibraryError: If the native library cannot be used.
        """
        # Before the library, so that this fault is named whether or not there is one.
        if proxy is not None:
            _check_proxy(proxy)
        self._library = load_library()
        settings_dict: dict[str, object] = {
            "version": SETTINGS_VERSION,
            "ignore_certificate_errors_spki_list": list(ignore_certificate_errors_spki_list),
            "additional_trust_anchors": list(additional_trust_anchors),
            "accept_language": accept_language,
            "user_agent": user_agent,
            "origins_to_force_quic_on": list(origins_to_force_quic_on),
            "host_resolver_rules": host_resolver_rules,
            "accept_ch_via_alps": dict(accept_ch_via_alps or {}),
        }
        if dns_nameserver is not None:
            settings_dict["dns_nameserver"] = dns_nameserver
        if refused_connects:
            settings_dict["refused_connects"] = refused_connects
        if proxy is not None:
            settings_dict["proxy"] = proxy
        if http_cache is not None:
            settings_dict["http_cache"] = http_cache
        if network_quality is not None:
            settings_dict["network_quality"] = network_quality.to_setting()
        settings = json.dumps(settings_dict).encode()
        self._handle = ctypes.POINTER(MittoSession)()
        net_error: int = self._library.mitto_session_create(settings, len(settings), ctypes.byref(self._handle))
        if net_error == _ERR_INVALID_ARGUMENT:
            shown = settings_dict if proxy is None else {**settings_dict, "proxy": "<not shown>"}
            raise ValueError(f"the native core refused the settings {shown!r} with net error {net_error}")
        if net_error != 0:
            raise _cache_error(self._library, http_cache, net_error)

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
        self.detach()()

    def detach(self) -> Callable[[], None]:
        """Refuse every later call at once, as a closed Session does, end every wait_for_event(), and return what closes it.

        What it returns can block for a moment, on the network thread and any cache on disk, so that a caller who cannot
        block can call it elsewhere. A thread that waits in wait_for_event() has to end before that is called. A Session
        that has been detached already gets back something that does nothing.
        """
        handle, self._handle = self._handle, ctypes.POINTER(MittoSession)()
        self._library.mitto_session_end_waits(handle)
        return functools.partial(self._library.mitto_session_destroy, handle)

    def list_cookies(self) -> bytes:
        """Return all this Session's cookies, as the JSON that the C API's mitto_session_list_cookies() writes.

        Raises:
            ValueError: If this Session has been closed.
        """
        self._require_open()
        buffer = ctypes.POINTER(ctypes.c_char)()
        size = ctypes.c_size_t()
        net_error: int = self._library.mitto_session_list_cookies(
            self._handle, ctypes.byref(buffer), ctypes.byref(size)
        )
        if net_error != 0:
            raise ValueError(f"the native core refused to list the cookies with net error {net_error}")
        try:
            return ctypes.string_at(buffer, size.value)
        finally:
            self._library.mitto_cookies_free(buffer)

    def set_cookies(self, cookies: bytes) -> None:
        """Put cookies in this Session's store, each replacing any with the same name, domain, path and partition key.

        Args:
            cookies: The JSON that the C API's mitto_session_set_cookies() reads.

        Raises:
            ValueError: If the native core refuses them, in which case none is set, or this Session has been closed. The
                message leaves out the cookies, which may be secrets.
        """
        self._require_open()
        net_error: int = self._library.mitto_session_set_cookies(self._handle, cookies, len(cookies))
        if net_error != 0:
            raise ValueError(f"the native core refused the cookies with net error {net_error}")

    def delete_cookies(self, cookie_filter: bytes) -> int:
        """Delete this Session's cookies that match a filter, and return how many went.

        Args:
            cookie_filter: The JSON that the C API's mitto_session_delete_cookies() reads.

        Raises:
            ValueError: If the native core refuses the filter, or this Session has been closed.
        """
        self._require_open()
        deleted = ctypes.c_uint32()
        net_error: int = self._library.mitto_session_delete_cookies(
            self._handle, cookie_filter, len(cookie_filter), ctypes.byref(deleted)
        )
        if net_error != 0:
            raise ValueError(f"the native core refused the filter of cookies with net error {net_error}")
        return deleted.value

    def _require_open(self) -> None:
        """Raise a ValueError if this Session has been closed, as a closed file does."""
        if not self._handle:
            raise ValueError("this Session has been closed")

    def event_fd(self) -> int:
        """Return a file descriptor that is readable while any of this Session's requests has an event waiting.

        This is for a caller that cannot block, such as an event loop. Get it before starting requests, because only
        events that arrive after the first call count. The Session owns it, so it must not be closed or read directly.

        Raises:
            OSError: If there is no room for another file descriptor, and always on Windows, where a Session has none.
        """
        fd = ctypes.c_int32()
        net_error: int = self._library.mitto_session_event_fd(self._handle, ctypes.byref(fd))
        if net_error != 0:
            raise OSError(f"the native core has no file descriptor to give, with net error {net_error}")
        return fd.value

    def wait_for_event(self, timeout: float | None = None) -> bool:
        """Block, with the GIL released, until next_ready() would name a request or Page, for a caller with no event_fd().

        It returns once each time the list goes from empty to naming a handle, and then waits until next_ready() has
        emptied it and another event has arrived, so a thread that hands each return to an event loop sleeps until
        the loop has caught up. Only events that arrive after the first call to this or to event_fd() count, so make one
        before starting requests, with a timeout of 0 so as not to block.

        Args:
            timeout: How many seconds to wait at most, or None to wait for as long as it takes.

        Returns:
            True if a handle waits to be named. False if none came in time, and at once after detach() or close().
        """
        timeout_ms = -1 if timeout is None else math.ceil(timeout * 1000)
        net_error: int = self._library.mitto_session_wait_for_event(self._handle, timeout_ms)
        return net_error == 0

    def next_ready(self) -> int | None:
        """Take the next waiting event's entry off the list behind event_fd() and wait_for_event().

        A request and a Page that are open at the same time never share an identity, so the identity alone says which
        it names.

        Returns:
            The identity of the request or Page whose event is waiting, such as NativeRequest.identity, or None if no
            event is waiting. One with several events waiting comes up once for each. It may have been closed, or its
            identity may be in use by a newer one that has no event yet, so expect its next event to be missing at
            times.
        """
        kind = ctypes.c_int32()
        handle = ctypes.c_void_p()
        self._library.mitto_session_next_ready(self._handle, ctypes.byref(kind), ctypes.byref(handle))
        return handle.value

    def open_page(
        self,
        start: PageStart,
        *,
        url: str | None = None,
        requests: Mapping[str, PageRequestTemplate] | None = None,
    ) -> NativePage:
        """Open a Page on this Session: start its process, and return without waiting for it to start.

        Starting the process takes a few milliseconds, which this blocks for.

        Args:
            start: What the Page starts with, as the Session's Machine and Persona say.
            url: Where the Page navigates once it opens, through this Session, whose PageNavigated and PageLoaded events
                say how that went, or None to stay at about:blank.
            requests: What a request of each Request Kind sends, by the kind's value: "navigation", needed with `url`,
                and "script" and "fetch", for the Page's own requests, and "fetch_with_body" for a fetch() with a body.

        Raises:
            ValueError: If this Session has been closed, or the native core refuses `start`. A time zone or locale that
                ICU does not know ends the Page, which its first call says.
            NativeLibraryError: If no Page program lies beside the native library, which means that this build of mitto
                has no Runtime.
            OSError: If the Page's process cannot start.
        """
        self._require_open()
        options: dict[str, object] = {"version": PAGE_OPTIONS_VERSION, "start": start.to_option()}
        if url is not None:
            encode_url(url)
            options["url"] = url
        if requests is not None:
            options["requests"] = {kind: template.to_option() for kind, template in requests.items()}
        encoded = json.dumps(options).encode()
        handle = ctypes.POINTER(MittoPage)()
        net_error: int = self._library.mitto_page_open(self._handle, encoded, len(encoded), ctypes.byref(handle))
        if net_error == _ERR_FILE_NOT_FOUND:
            raise NativeLibraryError(
                f"the Runtime is not in this build of mitto: no {PAGE_PROGRAM_NAME} lies beside the native library"
            )
        if net_error == _ERR_INVALID_ARGUMENT:
            raise ValueError(f"the native core refused what a Page starts with: {start}")
        if net_error != 0:
            name: bytes = self._library.mitto_net_error_name(net_error)
            raise OSError(f"the process of a Page cannot start: {name.decode()} ({net_error})")
        return NativePage(self._library, handle)

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
        body: bytes | StreamBody | None = None,
        follow_redirects: bool = True,
        fetch: FetchParams | None = None,
    ) -> NativeRequest:
        """Start a request for `url` and return without waiting for it.

        Args:
            url: The URL to fetch.
            headers: The (name, value) pairs to send, in the order they are to reach the wire, encoded as Latin-1. A
                name may appear only once, ignoring case. //net adds its own headers as it does in Chrome, such as Host
                before these and Accept-Encoding after them, unless they are among these. Referer cannot be among these,
                because //net only sends the one that `referrer` or `fetch` gives it.
            method: The HTTP method, sent as it is written here. Chrome never sends CONNECT, TRACE or TRACK, so they are
                refused.
            priority: The priority of the request.
            priority_incremental: Whether the response can be used as it arrives. With `priority`, this decides the
                HTTP/2 weight, the priority header and the HTTP/3 PRIORITY_UPDATE.
            referrer: The URL to send as the Referer header on each hop. //net puts it after `headers` and before the
                headers it adds, and leaves out any username, password and fragment. None leaves it to `fetch`, or sends
                none if there is none.
            timeout: The timers of the request. One that runs out arrives as the Finished event.
            body: What to send as the request's body. The native core copies bytes before this returns. A stream goes as the
                caller of NativeRequest.send_body() gives it, on each BodyWanted event. None sends no body, and //net still
                sends a Content-Length of 0 with POST and PUT. //net adds a Content-Length for a body, also an empty one, but
                for a stream with no size.
            follow_redirects: False makes a redirect the response, with an empty body, because //net does not read the body
                of a redirect. A redirect to a URL that is not valid still ends the request with ERR_INVALID_REDIRECT.
            fetch: What the request says of itself, from which the native core sets the headers that follow the Initiator,
                at the start and on each hop. None sends what `headers` and `referrer` say and no more, and the request
                counts as a navigation that the browser started.

        Raises:
            ValueError: If `url`, `referrer` or the initiator in `fetch` holds a NUL character or cannot be encoded as UTF-8,
                which the message names, or a header cannot be encoded as Latin-1.
            RequestError: If the native core refuses the request with ERR_INVALID_ARGUMENT, because of an invalid header or
                method, a `referrer` that //net cannot send, or an initiator that is not a valid URL. What goes wrong later
                arrives as the Finished event.
        """
        encoded_url = encode_url(url)
        options: dict[str, object] = {
            "version": REQUEST_OPTIONS_VERSION,
            "method": method,
            "priority": int(priority),
            "priority_incremental": priority_incremental,
            "follow_redirects": follow_redirects,
            **timeout_options(timeout),
        }
        if referrer is not None:
            encode_url(referrer, "referrer")
            options["referrer"] = referrer
        if fetch is not None:
            options["fetch"] = fetch.to_option()
        stream = body if isinstance(body, StreamBody) else None
        if stream is not None:
            options["stream_body"] = True
            if stream.size is not None:
                options["stream_body_size"] = stream.size
        encoded_options = json.dumps(options).encode()
        # This list keeps the bytes alive during the call, since the structs only point to them.
        encoded_headers = [(name.encode("latin-1"), value.encode("latin-1")) for name, value in headers]
        native_headers = (MittoHeader * len(encoded_headers))(
            *(MittoHeader(name, len(name), value, len(value)) for name, value in encoded_headers)
        )

        bytes_of_body = None if isinstance(body, StreamBody) else body
        handle = ctypes.POINTER(MittoRequest)()
        net_error: int = self._library.mitto_request_start(
            self._handle,
            encoded_url,
            native_headers,
            len(native_headers),
            encoded_options,
            len(encoded_options),
            bytes_of_body,
            0 if bytes_of_body is None else len(bytes_of_body),
            ctypes.byref(handle),
        )
        if net_error != 0:
            raise _request_error(self._library, url, net_error, method)
        return NativeRequest(self._library, handle, url, method, None if stream is None else stream.chunks)

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
        body: bytes | StreamBody | None = None,
        follow_redirects: bool = True,
        fetch: FetchParams | None = None,
    ) -> NativeResponse:
        """Fetch `url` and block until the whole response has arrived, with the GIL released while it waits.

        The arguments are those of start().

        Raises:
            ValueError: If `url`, `referrer` or the initiator in `fetch` holds a NUL character or cannot be encoded as UTF-8,
                or a header cannot be encoded as Latin-1.
            RequestError: If the request ends without a response, of the subclass that suits its net error. An HTTP error
                status such as 404 is a response. An invalid header or method, a `referrer` that //net cannot send, or an
                initiator that is not a valid URL, ends the request with ERR_INVALID_ARGUMENT.
        """
        with self.start(
            url, headers, method, priority, priority_incremental, referrer, timeout, body, follow_redirects, fetch
        ) as request:
            return _collect_response(request)


def wait_for_response(request: NativeRequest) -> tuple[ResponseStarted, tuple[RedirectFollowed, ...]]:
    """Block until the final response has started, giving `request` its stream body meanwhile.

    Returns:
        The final response, and the redirects that led to it, the first one first.

    Raises:
        RequestError: If the request ends without a response.
    """
    history: list[RedirectFollowed] = []
    while not isinstance(event := request.next_event(), ResponseStarted):
        if isinstance(event, Finished):
            raise request.error(event.net_error)
        if isinstance(event, RedirectFollowed):
            history.append(event)
        if isinstance(event, BodyWanted):
            request.send_body()
    return event, tuple(history)


def iter_body(request: NativeRequest) -> Iterator[bytes]:
    """Yield the response's body as it arrives, once wait_for_response() has returned. Each part is read when asked for.

    Raises:
        RequestError: If the request ends before the end of the body.
    """
    while True:
        # This stops at the point where a read asks for more, which leads to the next event.
        while chunk := request.read():
            yield chunk
        while not isinstance(event := request.next_event(), BodyAvailable | Finished):
            if isinstance(event, BodyWanted):
                request.send_body()
        if isinstance(event, Finished):
            if event.net_error != 0:
                raise request.error(event.net_error)
            return


def _collect_response(request: NativeRequest) -> NativeResponse:
    """Drive `request` to its end and return the whole response.

    Raises:
        RequestError: If the request ends without a response.
    """
    started, history = wait_for_response(request)
    return finished_response(started, history, b"".join(iter_body(request)))


def finished_response(started: ResponseStarted, history: tuple[RedirectFollowed, ...], body: bytes) -> NativeResponse:
    """Return the response that has started with `started`, after the redirects in `history`, and has `body`."""
    return NativeResponse(
        status=started.status,
        protocol=started.protocol,
        url=started.url,
        headers=started.headers,
        body=body,
        history=history,
        from_cache=started.from_cache,
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
                from_cache=bool(native.from_cache),
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
        case EventType.BODY_WANTED:
            return BodyWanted()


def _copy_page_event(native: MittoPageEvent) -> NativePageEvent:
    """Return a copy of a Page's event from the native core, which owns its buffers.

    The native core checks that what the Page process writes is UTF-8, and its own messages are, so nothing is replaced.
    """
    # By its size, since the Page process may write a NUL character in it.
    message = ctypes.string_at(native.message, native.message_size).decode(errors="replace") if native.message else ""
    match PageEventType(native.type):
        case PageEventType.EVALUATED:
            result = ctypes.string_at(native.json, native.json_size).decode(errors="replace") if native.json else ""
            return PageEvaluated(call=native.call, error=PageErrorCode(native.error), result=result, message=message)
        case PageEventType.ENDED:
            return PageEnded(error=PageErrorCode(native.error), message=message)
        case PageEventType.CONSOLE:
            return ConsoleMessage(level=ConsoleLevel(native.level).name.lower(), text=message)
        case PageEventType.UNCAUGHT_ERROR:
            return UncaughtError(text=message)
        case PageEventType.KNOWN_GAP:
            return KnownGap(name=message)
        case PageEventType.NAVIGATED:
            url = ctypes.string_at(native.url, native.url_size).decode(errors="replace") if native.url else ""
            return PageNavigated(url=url, status=native.status, net_error=native.net_error)
        case PageEventType.LOADED:
            return PageLoaded()


def _parse_headers(block: bytes) -> tuple[tuple[str, str], ...]:
    """Split the C API's "name: value\\r\\n" lines into pairs.

    Only CRLF ends a line. str.splitlines() would also split on characters that a value may hold, such as 0x85.
    """
    lines = block.decode("latin-1").split("\r\n")
    return tuple((name, value) for name, _, value in (line.partition(": ") for line in lines if line))
