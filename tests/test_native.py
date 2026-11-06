"""Tests for the ctypes loader of the native core.

Tests of finding and rejecting libraries use temporary files and stub libraries. Tests that take the native_library
fixture call the real native core and are skipped when this machine has none.
"""

import ctypes
import http.server
import re
import shutil
import subprocess
import sys
import threading
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any, cast

import pytest

from mitto import _native
from tests.local_server import LocalHTTPServer

HEADER = Path(__file__).parents[1] / "native" / "include" / "mitto" / "mitto.h"

COMPLETE_STUB = """
#include <stdint.h>
uint32_t mitto_abi_version(void) {{ return {abi_version}; }}
int32_t mitto_session_create(const char* settings, unsigned long size, void** session) {{ return -2; }}
void mitto_session_destroy(void* session) {{}}
int32_t mitto_session_event_fd(void* session, int32_t* fd) {{ return -2; }}
int32_t mitto_session_next_ready_request(void* session, void** request) {{ return -2; }}
int32_t mitto_request_start(void* session, const char* url, const void* headers, unsigned long header_count,
                            const char* options, unsigned long options_size, void** request) {{ return -2; }}
int32_t mitto_request_next_event(void* request, int64_t timeout_ms, void* event) {{ return -2; }}
int32_t mitto_request_read(void* request, char* buffer, unsigned long size) {{ return -2; }}
void mitto_request_cancel(void* request) {{}}
void mitto_request_release(void* request) {{}}
void mitto_event_free(void* event) {{}}
"""

ERR_IO_PENDING = -1
ERR_INVALID_ARGUMENT = -4
ERR_INVALID_URL = -300

NO_SETTINGS = f'{{"version": {_native.SETTINGS_VERSION}}}'.encode()
NO_OPTIONS = f'{{"version": {_native.REQUEST_OPTIONS_VERSION}}}'.encode()

# How long a test waits for one event, in milliseconds.
EVENT_TIMEOUT_MS = 30_000

# The Base64 form of 32 bytes, the size of a SHA-256 hash.
SPKI_HASH = "A" * 43 + "="

# Parts of glibc, and libgcc. A manylinux wheel may depend on these, and a machine that runs it has them.
LINUX_SYSTEM_LIBRARIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libgcc_s.so.1",
    "libm.so.6",
    "libpthread.so.0",
    "libresolv.so.2",
    "librt.so.1",
    "linux-vdso.so.1",
}

# The NUL shows that the body is read by its size, not as a C string.
BODY = b"a body with a \x00 inside"


class StubServerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        self.send_response(203)
        self.send_header("X-Mitto-Test", "value")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def package_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Make the loader see an empty directory as the installed package, with no local build named."""
    directory = tmp_path / "package"
    directory.mkdir()
    monkeypatch.setattr(_native, "__file__", str(directory / "_native.py"))
    monkeypatch.delenv(_native.LIBRARY_ENV_VAR, raising=False)
    return directory


@pytest.fixture
def build_stub(tmp_path: Path) -> Callable[[str], Path]:
    """Return a function that compiles C source into a shared library, or skip if there is no C compiler."""
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("no C compiler")

    def build(source: str) -> Path:
        source_path = tmp_path / "stub.c"
        source_path.write_text(source)
        library = tmp_path / _native.LIBRARY_NAME
        subprocess.run([compiler, "-shared", "-fPIC", "-o", library, source_path], check=True)
        return library

    return build


@pytest.fixture
def server_url() -> Iterator[str]:
    """Run a local HTTP server and return its URL."""
    with LocalHTTPServer(("127.0.0.1", 0), StubServerHandler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}/"
        server.shutdown()


def header_constant(name: str) -> int:
    """Return the value of a #define in the C header."""
    match = re.search(rf"^#define {name} (\d+)$", HEADER.read_text(), re.MULTILINE)
    assert match is not None, name
    return int(match[1])


def create_session(library: ctypes.CDLL, settings: bytes) -> tuple[int, Any]:
    """Call mitto_session_create and return its result and the handle it stored."""
    handle = ctypes.POINTER(_native.MittoSession)()
    error: int = library.mitto_session_create(settings, len(settings), ctypes.byref(handle))
    return error, handle


def start_request(
    library: ctypes.CDLL,
    session: Any,
    url: bytes | None,
    headers: Any = None,
    header_count: int = 0,
    options: bytes | None = NO_OPTIONS,
) -> tuple[int, Any]:
    """Call mitto_request_start and return its result and the handle it stored."""
    # The handle starts out as something other than null, to show that a failed call stores null.
    handle = ctypes.pointer(_native.MittoRequest())
    error: int = library.mitto_request_start(
        session, url, headers, header_count, options, len(options or b""), ctypes.byref(handle)
    )
    return error, handle


def next_event(library: ctypes.CDLL, request: Any) -> _native.MittoEvent:
    """Take the next event, which the caller has to free."""
    event = _native.MittoEvent()
    assert library.mitto_request_next_event(request, EVENT_TIMEOUT_MS, ctypes.byref(event)) == 0
    return event


@pytest.fixture
def session_handle(native_library: ctypes.CDLL) -> Iterator[Any]:
    """Create a real Session and return its handle."""
    error, handle = create_session(native_library, NO_SETTINGS)
    assert error == 0
    yield handle
    native_library.mitto_session_destroy(handle)


@pytest.fixture
def request_handle(native_library: ctypes.CDLL, session_handle: Any, server_url: str) -> Iterator[Any]:
    """Start a request to the local server and return its handle."""
    error, handle = start_request(native_library, session_handle, server_url.encode())
    assert error == 0
    yield handle
    native_library.mitto_request_release(handle)


def test_abi_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_ABI_VERSION") == _native.ABI_VERSION


def test_settings_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_SETTINGS_VERSION") == _native.SETTINGS_VERSION


def test_request_options_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_REQUEST_OPTIONS_VERSION") == _native.REQUEST_OPTIONS_VERSION


@pytest.mark.parametrize("event_type", _native.EventType)
def test_event_type_matches_the_c_header(event_type: _native.EventType) -> None:
    assert header_constant(f"MITTO_EVENT_{event_type.name}") == event_type


@pytest.mark.parametrize("priority", _native.Priority)
def test_priority_matches_the_c_header(priority: _native.Priority) -> None:
    assert header_constant(f"MITTO_PRIORITY_{priority.name}") == priority


def test_find_library_returns_none_without_a_library(package_dir: Path) -> None:
    assert _native.find_library() is None


def test_find_library_finds_the_library_in_the_package(package_dir: Path) -> None:
    packaged = package_dir / _native.LIBRARY_NAME
    packaged.touch()

    assert _native.find_library() == packaged


def test_find_library_prefers_the_env_var_over_the_package(
    package_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (package_dir / _native.LIBRARY_NAME).touch()
    local_build = tmp_path / "local_build"
    local_build.touch()
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(local_build))

    assert _native.find_library() == local_build


def test_find_library_treats_an_empty_env_var_as_unset(package_dir: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, "")

    assert _native.find_library() is None


def test_find_library_makes_a_relative_env_var_absolute(
    package_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (tmp_path / "local_build").touch()
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, "local_build")

    found = _native.find_library()

    assert found is not None
    assert found.is_absolute()
    assert found.name == "local_build"


def test_find_library_rejects_an_env_var_naming_a_missing_file(
    package_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (package_dir / _native.LIBRARY_NAME).touch()
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(_native.NativeLibraryError, match=_native.LIBRARY_ENV_VAR):
        _native.find_library()


def test_load_library_without_a_library_names_the_env_var(package_dir: Path) -> None:
    with pytest.raises(_native.NativeLibraryError, match=_native.LIBRARY_ENV_VAR):
        _native.load_library()


def test_load_library_rejects_a_file_that_is_not_a_library(package_dir: Path) -> None:
    (package_dir / _native.LIBRARY_NAME).write_text("not a shared library")

    with pytest.raises(_native.NativeLibraryError, match="cannot use"):
        _native.load_library()


def test_load_library_rejects_a_library_without_the_api(
    build_stub: Callable[[str], Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(build_stub("int unrelated(void) { return 0; }")))

    with pytest.raises(_native.NativeLibraryError, match="cannot use"):
        _native.load_library()


def test_load_library_rejects_another_abi_version(
    build_stub: Callable[[str], Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    other_version = _native.ABI_VERSION + 1
    stub = build_stub(COMPLETE_STUB.format(abi_version=other_version))
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(stub))

    with pytest.raises(_native.NativeLibraryError, match=f"ABI version {other_version}"):
        _native.load_library()


def test_load_library_loads_the_library_once(
    build_stub: Callable[[str], Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    stub = build_stub(COMPLETE_STUB.format(abi_version=_native.ABI_VERSION))
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(stub))

    assert _native.load_library() is _native.load_library()


def test_native_library_has_the_expected_abi_version(native_library: ctypes.CDLL) -> None:
    assert native_library.mitto_abi_version() == _native.ABI_VERSION


@pytest.mark.skipif(sys.platform != "linux", reason="only Linux wheels have to avoid other system libraries")
@pytest.mark.usefixtures("native_library")
def test_native_library_needs_only_glibc_and_libgcc_on_linux() -> None:
    library = _native.find_library()
    assert library is not None
    ldd = subprocess.run(["ldd", library], check=True, capture_output=True, text=True).stdout
    assert {Path(line.split()[0]).name for line in ldd.splitlines()} <= LINUX_SYSTEM_LIBRARIES


def test_session_create_accepts_settings_with_only_a_version(native_library: ctypes.CDLL) -> None:
    error, handle = create_session(native_library, NO_SETTINGS)

    assert error == 0
    assert handle
    native_library.mitto_session_destroy(handle)


def test_session_create_accepts_an_spki_hash(native_library: ctypes.CDLL) -> None:
    settings = f'{{"version": 1, "ignore_certificate_errors_spki_list": ["{SPKI_HASH}"]}}'.encode()

    error, handle = create_session(native_library, settings)

    assert error == 0
    native_library.mitto_session_destroy(handle)


def test_session_create_accepts_an_accept_language(native_library: ctypes.CDLL) -> None:
    error, handle = create_session(native_library, b'{"version": 1, "accept_language": "en-US,en;q=0.9"}')

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize("origin", ["example.com:443", "capture.localhost:8445", "127.0.0.1:8445", "*"])
def test_session_create_accepts_an_origin_to_force_quic_on(native_library: ctypes.CDLL, origin: str) -> None:
    settings = f'{{"version": 1, "origins_to_force_quic_on": ["{origin}"]}}'.encode()

    error, handle = create_session(native_library, settings)

    assert error == 0
    native_library.mitto_session_destroy(handle)


def test_session_create_reads_the_settings_by_their_size(native_library: ctypes.CDLL) -> None:
    handle = ctypes.POINTER(_native.MittoSession)()

    error = native_library.mitto_session_create(NO_SETTINGS + b"not json", len(NO_SETTINGS), ctypes.byref(handle))

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize(
    "settings",
    [
        pytest.param(b"", id="empty"),
        pytest.param(b"not json", id="not-json"),
        pytest.param(b"[]", id="not-an-object"),
        pytest.param(b"{}", id="no-version"),
        pytest.param(b'{"version": 0}', id="another-version"),
        pytest.param(b'{"version": 1.5}', id="version-is-not-an-integer"),
        pytest.param(b'{"version": "1"}', id="version-is-a-string"),
        pytest.param(b'{"version": 1, "unknown": true}', id="unknown-key"),
        pytest.param(b'{"version": 1, "ignore_certificate_errors_spki_list": "x"}', id="list-is-a-string"),
        pytest.param(b'{"version": 1, "ignore_certificate_errors_spki_list": [1]}', id="entry-is-a-number"),
        pytest.param(b'{"version": 1, "ignore_certificate_errors_spki_list": ["!"]}', id="entry-is-not-base64"),
        pytest.param(b'{"version": 1, "ignore_certificate_errors_spki_list": ["AAAA"]}', id="entry-is-too-short"),
        pytest.param(b'{"version": 1, "accept_language": 1}', id="accept-language-is-a-number"),
        pytest.param(b'{"version": 1, "accept_language": "a\\r\\nb"}', id="crlf-in-accept-language"),
        pytest.param(b'{"version": 1, "accept_language": "a\\u0000b"}', id="nul-in-accept-language"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": "a.test:443"}', id="origins-is-a-string"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": [1]}', id="origin-is-a-number"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": [""]}', id="origin-is-empty"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a.test"]}', id="origin-has-no-port"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a.test:65536"]}', id="origin-has-a-large-port"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["https://a.test:443"]}', id="origin-has-a-scheme"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["A.test:443"]}', id="origin-is-not-canonical"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["[::1]:443"]}', id="origin-is-ipv6"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a\\u0000.test:443"]}', id="nul-in-origin"),
    ],
)
def test_session_create_refuses_invalid_settings(native_library: ctypes.CDLL, settings: bytes) -> None:
    # The handle starts out as something other than null, to show that the call stores null.
    handle = ctypes.pointer(_native.MittoSession())

    error = native_library.mitto_session_create(settings, len(settings), ctypes.byref(handle))

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_session_destroy_accepts_null(native_library: ctypes.CDLL) -> None:
    native_library.mitto_session_destroy(None)


def assert_zeroed(event: _native.MittoEvent) -> None:
    assert event.type == 0
    assert event.status == 0
    assert event.net_error == 0
    assert event.protocol is None
    assert event.url is None
    assert not event.headers
    assert event.headers_size == 0


def test_a_request_goes_through_its_events_and_free_zeroes_them(
    native_library: ctypes.CDLL, request_handle: Any, server_url: str
) -> None:
    buffer = ctypes.create_string_buffer(len(BODY) * 2)

    started = next_event(native_library, request_handle)
    assert started.type == _native.EventType.RESPONSE_STARTED
    assert started.status == 203
    assert started.protocol == b"http/1.0"
    assert started.url == server_url.encode()
    assert b"X-Mitto-Test: value\r\n" in ctypes.string_at(started.headers, started.headers_size)
    native_library.mitto_event_free(ctypes.byref(started))
    assert_zeroed(started)

    assert native_library.mitto_request_read(request_handle, buffer, len(buffer)) == ERR_IO_PENDING
    available = next_event(native_library, request_handle)
    assert available.type == _native.EventType.BODY_AVAILABLE
    assert available.protocol is None
    assert available.url is None
    assert not available.headers
    native_library.mitto_event_free(ctypes.byref(available))

    size = native_library.mitto_request_read(request_handle, buffer, len(buffer))
    assert ctypes.string_at(buffer, size) == BODY

    assert native_library.mitto_request_read(request_handle, buffer, len(buffer)) == ERR_IO_PENDING
    finished = next_event(native_library, request_handle)
    assert finished.type == _native.EventType.FINISHED
    assert finished.net_error == 0
    native_library.mitto_event_free(ctypes.byref(finished))

    assert native_library.mitto_request_read(request_handle, buffer, len(buffer)) == 0


def test_the_last_event_is_the_last(native_library: ctypes.CDLL, session_handle: Any) -> None:
    error, request = start_request(native_library, session_handle, b"not a url")
    assert error == 0
    finished = next_event(native_library, request)
    assert finished.type == _native.EventType.FINISHED

    # A negative timeout, so that this would hang if the call waited for another.
    after = _native.MittoEvent(type=1)
    assert native_library.mitto_request_next_event(request, -1, ctypes.byref(after)) == ERR_INVALID_ARGUMENT
    assert_zeroed(after)
    native_library.mitto_request_release(request)


def test_a_url_that_is_not_valid_finishes_with_the_net_error(native_library: ctypes.CDLL, session_handle: Any) -> None:
    error, request = start_request(native_library, session_handle, b"not a url")

    assert error == 0
    finished = next_event(native_library, request)
    assert finished.type == _native.EventType.FINISHED
    assert finished.net_error == ERR_INVALID_URL
    native_library.mitto_request_release(request)


def test_next_event_stores_nothing_when_no_event_is_waiting(native_library: ctypes.CDLL, request_handle: Any) -> None:
    started = next_event(native_library, request_handle)
    native_library.mitto_event_free(ctypes.byref(started))

    # The body waits to be asked for, so nothing can arrive until then.
    event = _native.MittoEvent(type=1)
    assert native_library.mitto_request_next_event(request_handle, 0, ctypes.byref(event)) == ERR_IO_PENDING
    assert_zeroed(event)
    event = _native.MittoEvent(type=1)
    assert native_library.mitto_request_next_event(request_handle, 1, ctypes.byref(event)) == ERR_IO_PENDING
    assert_zeroed(event)


def test_next_event_refuses_null_arguments(native_library: ctypes.CDLL, request_handle: Any) -> None:
    event = _native.MittoEvent(type=1)

    assert native_library.mitto_request_next_event(None, 0, ctypes.byref(event)) == ERR_INVALID_ARGUMENT
    assert_zeroed(event)
    assert native_library.mitto_request_next_event(request_handle, 0, None) == ERR_INVALID_ARGUMENT


def test_read_refuses_invalid_arguments(native_library: ctypes.CDLL, request_handle: Any) -> None:
    buffer = ctypes.create_string_buffer(1)

    assert native_library.mitto_request_read(None, buffer, 1) == ERR_INVALID_ARGUMENT
    assert native_library.mitto_request_read(request_handle, None, 1) == ERR_INVALID_ARGUMENT
    assert native_library.mitto_request_read(request_handle, buffer, 0) == ERR_INVALID_ARGUMENT


def test_null_requests_and_events_are_accepted_where_nothing_needs_doing(native_library: ctypes.CDLL) -> None:
    native_library.mitto_request_cancel(None)
    native_library.mitto_request_release(None)
    native_library.mitto_event_free(None)
    native_library.mitto_event_free(ctypes.byref(_native.MittoEvent()))


def test_event_fd_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    fd = ctypes.c_int32(1)

    assert native_library.mitto_session_event_fd(None, ctypes.byref(fd)) == ERR_INVALID_ARGUMENT
    assert fd.value == -1
    assert native_library.mitto_session_event_fd(session_handle, None) == ERR_INVALID_ARGUMENT


def test_next_ready_request_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    request = ctypes.pointer(_native.MittoRequest())

    assert native_library.mitto_session_next_ready_request(None, ctypes.byref(request)) == ERR_INVALID_ARGUMENT
    assert not request
    assert native_library.mitto_session_next_ready_request(session_handle, None) == ERR_INVALID_ARGUMENT


def test_next_ready_request_stores_null_when_nothing_is_waiting(
    native_library: ctypes.CDLL, session_handle: Any
) -> None:
    request = ctypes.pointer(_native.MittoRequest())

    assert native_library.mitto_session_next_ready_request(session_handle, ctypes.byref(request)) == ERR_IO_PENDING
    assert not request


def test_request_start_takes_the_url_as_bytes(native_library: ctypes.CDLL, session_handle: Any) -> None:
    with pytest.raises(ctypes.ArgumentError):
        start_request(native_library, session_handle, cast(bytes, "http://127.0.0.1/"))


def test_request_start_refuses_a_null_session(native_library: ctypes.CDLL, server_url: str) -> None:
    error, handle = start_request(native_library, None, server_url.encode())

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_refuses_a_null_url(native_library: ctypes.CDLL, session_handle: Any) -> None:
    error, handle = start_request(native_library, session_handle, None)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_refuses_null_headers_with_a_count(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), header_count=1)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_refuses_null_options(native_library: ctypes.CDLL, session_handle: Any, server_url: str) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), options=None)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_refuses_a_null_result(native_library: ctypes.CDLL, session_handle: Any, server_url: str) -> None:
    error = native_library.mitto_request_start(
        session_handle, server_url.encode(), None, 0, NO_OPTIONS, len(NO_OPTIONS), None
    )

    assert error == ERR_INVALID_ARGUMENT


def test_request_start_reads_the_options_by_their_size(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    handle = ctypes.POINTER(_native.MittoRequest)()

    error = native_library.mitto_request_start(
        session_handle, server_url.encode(), None, 0, NO_OPTIONS + b"not json", len(NO_OPTIONS), ctypes.byref(handle)
    )

    assert error == 0
    native_library.mitto_request_release(handle)


@pytest.mark.parametrize(
    "options",
    [
        pytest.param(b'{"version": 1, "method": "HEAD"}', id="method"),
        pytest.param(b'{"version": 1, "method": "lowercase"}', id="method-is-sent-as-written"),
        pytest.param(b'{"version": 1, "priority": 0}', id="lowest-value-of-priority"),
        pytest.param(b'{"version": 1, "priority": 5}', id="highest-value-of-priority"),
        pytest.param(b'{"version": 1, "priority_incremental": false}', id="priority-incremental"),
        pytest.param(b'{"version": 1, "referrer": "https://a.test/page"}', id="referrer"),
    ],
)
def test_request_start_accepts_valid_options(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str, options: bytes
) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), options=options)

    assert error == 0
    native_library.mitto_request_release(handle)


@pytest.mark.parametrize(
    "options",
    [
        pytest.param(b"", id="empty"),
        pytest.param(b"not json", id="not-json"),
        pytest.param(b"[]", id="not-an-object"),
        pytest.param(b"{}", id="no-version"),
        pytest.param(b'{"version": 0}', id="another-version"),
        pytest.param(b'{"version": 1.5}', id="version-is-not-an-integer"),
        pytest.param(b'{"version": "1"}', id="version-is-a-string"),
        pytest.param(b'{"version": 1, "unknown": true}', id="unknown-key"),
        pytest.param(b'{"version": 1, "method": 1}', id="method-is-a-number"),
        pytest.param(b'{"version": 1, "method": ""}', id="method-is-empty"),
        pytest.param(b'{"version": 1, "method": "G T"}', id="space-in-method"),
        pytest.param(b'{"version": 1, "method": "GET\\r\\nX: y"}', id="crlf-in-method"),
        pytest.param(b'{"version": 1, "method": "GET\\u0000"}', id="nul-in-method"),
        pytest.param(b'{"version": 1, "method": "CONNECT"}', id="connect"),
        pytest.param(b'{"version": 1, "method": "trace"}', id="trace-in-another-case"),
        pytest.param(b'{"version": 1, "method": "TRACK"}', id="track"),
        pytest.param(b'{"version": 1, "priority": -1}', id="priority-below-the-range"),
        pytest.param(b'{"version": 1, "priority": 6}', id="priority-above-the-range"),
        pytest.param(b'{"version": 1, "priority": 1.5}', id="priority-is-not-an-integer"),
        pytest.param(b'{"version": 1, "priority": "LOW"}', id="priority-is-a-string"),
        pytest.param(b'{"version": 1, "priority_incremental": 1}', id="priority-incremental-is-a-number"),
        pytest.param(b'{"version": 1, "referrer": null}', id="referrer-is-null"),
        pytest.param(b'{"version": 1, "referrer": 1}', id="referrer-is-a-number"),
        pytest.param(b'{"version": 1, "referrer": ""}', id="referrer-is-empty"),
        pytest.param(b'{"version": 1, "referrer": "not a url"}', id="referrer-is-not-a-url"),
        pytest.param(b'{"version": 1, "referrer": "about:blank"}', id="referrer-has-another-scheme"),
    ],
)
def test_request_start_refuses_invalid_options(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str, options: bytes
) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), options=options)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


@pytest.mark.parametrize(
    "headers",
    [
        pytest.param([(b"", b"value")], id="empty-name"),
        pytest.param([(b"Bad Name", b"value")], id="space-in-name"),
        pytest.param([(b"X-Name\0", b"value")], id="nul-in-name"),
        pytest.param([(b"X-Name", b"a\0b")], id="nul-in-value"),
        pytest.param([(b"X-Name", b"a\r\nX-Injected: b")], id="crlf-in-value"),
        pytest.param([(b"X-Name", b"1"), (b"x-name", b"2")], id="repeated-name"),
        pytest.param([(b"Referer", b"http://127.0.0.1/")], id="referer"),
        pytest.param([(b"rEFERER", b"http://127.0.0.1/")], id="referer-in-another-case"),
    ],
)
def test_request_start_refuses_invalid_headers(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str, headers: list[tuple[bytes, bytes]]
) -> None:
    native_headers = (_native.MittoHeader * len(headers))(
        *(_native.MittoHeader(name, len(name), value, len(value)) for name, value in headers)
    )

    error, handle = start_request(
        native_library, session_handle, server_url.encode(), native_headers, len(native_headers)
    )

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_refuses_a_null_header_name(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    native_headers = (_native.MittoHeader * 1)(_native.MittoHeader(None, 0, b"value", 5))

    error, handle = start_request(native_library, session_handle, server_url.encode(), native_headers, 1)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle
