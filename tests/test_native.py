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
from typing import Any

import pytest

from mitto import _native

HEADER = Path(__file__).parents[1] / "native" / "include" / "mitto" / "mitto.h"

COMPLETE_STUB = """
#include <stdint.h>
uint32_t mitto_abi_version(void) {{ return {abi_version}; }}
int32_t mitto_session_create(const char* settings, unsigned long size, void** session) {{ return -2; }}
void mitto_session_destroy(void* session) {{}}
int32_t mitto_session_request(void* session, const char* url, const void* headers, unsigned long header_count,
                              int32_t priority, void* response) {{ return -2; }}
void mitto_response_free(void* response) {{}}
"""

ERR_INVALID_ARGUMENT = -4
ERR_INVALID_URL = -300

NO_SETTINGS = f'{{"version": {_native.SETTINGS_VERSION}}}'.encode()

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
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), StubServerHandler) as server:
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


@pytest.fixture
def session_handle(native_library: ctypes.CDLL) -> Iterator[Any]:
    """Create a real Session and return its handle."""
    error, handle = create_session(native_library, NO_SETTINGS)
    assert error == 0
    yield handle
    native_library.mitto_session_destroy(handle)


def test_abi_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_ABI_VERSION") == _native.ABI_VERSION


def test_settings_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_SETTINGS_VERSION") == _native.SETTINGS_VERSION


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


def test_request_fills_the_response_and_free_zeroes_it(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, server_url.encode(), None, 0, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == 0
    try:
        assert response.status == 203
        assert response.protocol == b"http/1.0"
        assert b"X-Mitto-Test: value\r\n" in ctypes.string_at(response.headers, response.headers_size)
        assert ctypes.string_at(response.body, response.body_size) == BODY
    finally:
        native_library.mitto_response_free(ctypes.byref(response))
    assert response.status == 0
    assert response.protocol is None
    assert not response.headers
    assert response.headers_size == 0
    assert not response.body
    assert response.body_size == 0


def test_request_failure_returns_the_net_error_and_leaves_the_response_zeroed(
    native_library: ctypes.CDLL, session_handle: Any
) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, b"not a url", None, 0, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == ERR_INVALID_URL
    assert response.status == 0
    assert response.protocol is None
    assert not response.headers
    assert not response.body


def test_request_takes_the_url_as_bytes(native_library: ctypes.CDLL, session_handle: Any) -> None:
    response = _native.MittoResponse()

    with pytest.raises(ctypes.ArgumentError):
        native_library.mitto_session_request(
            session_handle, "http://127.0.0.1/", None, 0, _native.Priority.LOWEST, ctypes.byref(response)
        )


def test_request_refuses_a_null_session(native_library: ctypes.CDLL, server_url: str) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        None, server_url.encode(), None, 0, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == ERR_INVALID_ARGUMENT


def test_request_refuses_a_null_url(native_library: ctypes.CDLL, session_handle: Any) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, None, None, 0, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == ERR_INVALID_ARGUMENT


def test_request_refuses_null_headers_with_a_count(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, server_url.encode(), None, 1, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == ERR_INVALID_ARGUMENT


@pytest.mark.parametrize("priority", [-1, 6])
def test_request_refuses_an_unknown_priority(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str, priority: int
) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, server_url.encode(), None, 0, priority, ctypes.byref(response)
    )

    assert error == ERR_INVALID_ARGUMENT


@pytest.mark.parametrize(
    "headers",
    [
        pytest.param([(b"", b"value")], id="empty-name"),
        pytest.param([(b"Bad Name", b"value")], id="space-in-name"),
        pytest.param([(b"X-Name\0", b"value")], id="nul-in-name"),
        pytest.param([(b"X-Name", b"a\0b")], id="nul-in-value"),
        pytest.param([(b"X-Name", b"a\r\nX-Injected: b")], id="crlf-in-value"),
        pytest.param([(b"X-Name", b"1"), (b"x-name", b"2")], id="repeated-name"),
    ],
)
def test_request_refuses_invalid_headers(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str, headers: list[tuple[bytes, bytes]]
) -> None:
    native_headers = (_native.MittoHeader * len(headers))(
        *(_native.MittoHeader(name, len(name), value, len(value)) for name, value in headers)
    )
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle,
        server_url.encode(),
        native_headers,
        len(native_headers),
        _native.Priority.LOWEST,
        ctypes.byref(response),
    )

    assert error == ERR_INVALID_ARGUMENT


def test_request_refuses_a_null_header_name(native_library: ctypes.CDLL, session_handle: Any, server_url: str) -> None:
    native_headers = (_native.MittoHeader * 1)(_native.MittoHeader(None, 0, b"value", 5))
    response = _native.MittoResponse()

    error = native_library.mitto_session_request(
        session_handle, server_url.encode(), native_headers, 1, _native.Priority.LOWEST, ctypes.byref(response)
    )

    assert error == ERR_INVALID_ARGUMENT


def test_free_accepts_null_and_a_zeroed_response(native_library: ctypes.CDLL) -> None:
    native_library.mitto_response_free(None)
    native_library.mitto_response_free(ctypes.byref(_native.MittoResponse()))
