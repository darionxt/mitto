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
int32_t mitto_get(const char* url, void* response) {{ return -2; }}
void mitto_response_free(void* response) {{}}
"""

ERR_INVALID_URL = -300

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


def test_abi_version_matches_the_c_header() -> None:
    match = re.search(r"^#define MITTO_ABI_VERSION (\d+)$", HEADER.read_text(), re.MULTILINE)

    assert match is not None
    assert int(match[1]) == _native.ABI_VERSION


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


def test_get_fills_the_response_and_free_zeroes_it(native_library: ctypes.CDLL, server_url: str) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_get(server_url.encode(), ctypes.byref(response))

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


def test_get_failure_returns_the_net_error_and_leaves_the_response_zeroed(native_library: ctypes.CDLL) -> None:
    response = _native.MittoResponse()

    error = native_library.mitto_get(b"not a url", ctypes.byref(response))

    assert error == ERR_INVALID_URL
    assert response.status == 0
    assert response.protocol is None
    assert not response.headers
    assert not response.body


def test_get_takes_the_url_as_bytes(native_library: ctypes.CDLL) -> None:
    response = _native.MittoResponse()

    with pytest.raises(ctypes.ArgumentError):
        native_library.mitto_get("http://127.0.0.1/", ctypes.byref(response))


def test_free_accepts_null_and_a_zeroed_response(native_library: ctypes.CDLL) -> None:
    native_library.mitto_response_free(None)
    native_library.mitto_response_free(ctypes.byref(_native.MittoResponse()))
