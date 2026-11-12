"""Tests for the ctypes loader of the native core.

Tests of finding and rejecting libraries use temporary files and stub libraries. Tests that take the native_library
fixture call the real native core and are skipped when this machine has none.
"""

import concurrent.futures
import ctypes
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any, cast

import pytest

from mitto import _native
from tests.local_server import LocalHTTPServer
from tests.pe_file import PeFile

NATIVE = Path(__file__).parents[1] / "native"
HEADER = NATIVE / "include" / "mitto" / "mitto.h"
_SETTINGS_FUZZ_SEEDS = NATIVE / "test" / "session_settings_corpus"

COMPLETE_STUB = """
#include <stdint.h>
uint32_t mitto_abi_version(void) {{ return {abi_version}; }}
const char* mitto_net_error_name(int32_t net_error) {{ return "ERR_STUB"; }}
int32_t mitto_session_create(const char* settings, unsigned long size, void** session) {{ return -2; }}
void mitto_session_destroy(void* session) {{}}
int32_t mitto_session_list_cookies(void* session, char** cookies, unsigned long* size) {{ return -2; }}
int32_t mitto_session_set_cookies(void* session, const char* cookies, unsigned long size) {{ return -2; }}
int32_t mitto_session_delete_cookies(void* session, const char* filter, unsigned long size, uint32_t* deleted) {{ return -2; }}
void mitto_cookies_free(char* cookies) {{}}
int32_t mitto_session_event_fd(void* session, int32_t* fd) {{ return -2; }}
int32_t mitto_session_next_ready(void* session, int32_t* kind, void** handle) {{ return -2; }}
int32_t mitto_session_wait_for_event(void* session, int64_t timeout_ms) {{ return -2; }}
void mitto_session_end_waits(void* session) {{}}
int32_t mitto_request_start(void* session, const char* url, const void* headers, unsigned long header_count,
                            const char* options, unsigned long options_size, const char* body,
                            unsigned long body_size, void** request) {{ return -2; }}
int32_t mitto_request_next_event(void* request, int64_t timeout_ms, void* event) {{ return -2; }}
int32_t mitto_request_read(void* request, char* buffer, unsigned long size) {{ return -2; }}
int32_t mitto_request_write_body(void* request, const char* data, unsigned long size) {{ return -2; }}
int32_t mitto_request_end_body(void* request) {{ return -2; }}
void mitto_request_cancel(void* request) {{}}
void mitto_request_release(void* request) {{}}
void mitto_event_free(void* event) {{}}
int32_t mitto_page_open(void* session, const char* options, unsigned long size, void** page) {{ return -2; }}
int32_t mitto_page_process_id(void* page, int64_t* process_id) {{ return -2; }}
int32_t mitto_page_evaluate(void* page, const char* script, unsigned long size, uint64_t* call) {{ return -2; }}
int32_t mitto_page_next_event(void* page, int64_t timeout_ms, void* event) {{ return -2; }}
void mitto_page_event_free(void* event) {{}}
void mitto_page_end_waits(void* page) {{}}
void mitto_page_close(void* page) {{}}
"""

ERR_IO_PENDING = -1
ERR_ABORTED = -3
ERR_INVALID_ARGUMENT = -4
ERR_UNSAFE_PORT = -312
ERR_NOT_IMPLEMENTED = -11
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

# The C runtimes that a program built with Visual Studio can load, which Chromium's build links into the library
# instead.
C_RUNTIMES = re.compile(r"(vcruntime|msvcp|concrt|vccorlib|ucrtbase|api-ms-win-crt-).*")
# Names that Windows' loader maps to one of its own DLLs, such as api-ms-win-core-synch-l1-2-0.dll, with no file of
# that name.
WINDOWS_API_SET = re.compile(r"api-ms-win-core-.*")

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


def c_api() -> set[str]:
    """Return the names of the functions that the C header declares."""
    return set(re.findall(r"^MITTO_EXPORT [^(]*?\b(\w+)\(", HEADER.read_text(), re.MULTILINE))


def exported_names(library: Path) -> set[str]:
    """Return the names of the functions that `library` exports, or skip if this Host's tool for them is missing."""
    if sys.platform == "win32":
        return set(PeFile(library).exported_names())
    nm = shutil.which("nm")
    if nm is None:
        pytest.skip("no nm")
    # Defined and external: what another library or program can link to.
    flags = ["-g", "-U", "-j"] if sys.platform == "darwin" else ["-D", "--defined-only", "--format=just-symbols"]
    names = subprocess.run([nm, *flags, library], check=True, capture_output=True, text=True).stdout.split()
    return {name.removeprefix("_") if sys.platform == "darwin" else name for name in names}


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
    body: bytes | None = None,
    body_size: int | None = None,
) -> tuple[int, Any]:
    """Call mitto_request_start and return its result and the handle it stored."""
    # The handle starts out as something other than null, to show that a failed call stores null.
    handle = ctypes.pointer(_native.MittoRequest())
    error: int = library.mitto_request_start(
        session,
        url,
        headers,
        header_count,
        options,
        len(options or b""),
        body,
        len(body or b"") if body_size is None else body_size,
        ctypes.byref(handle),
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


def test_each_exports_list_names_the_c_api() -> None:
    mac = (NATIVE / "mitto_core.exports").read_text().split()
    linux = re.findall(r"^    (\w+);$", (NATIVE / "mitto_core.lds").read_text(), re.MULTILINE)
    windows = (NATIVE / "mitto_core.def").read_text().split()

    assert {"mitto_abi_version", "mitto_session_create", "mitto_event_free"} <= c_api()
    assert {name.removeprefix("_") for name in mac} == c_api()
    assert set(linux) == c_api()
    assert windows[0] == "EXPORTS"
    assert set(windows[1:]) == c_api()


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


def test_page_options_version_matches_the_c_header() -> None:
    assert header_constant("MITTO_PAGE_OPTIONS_VERSION") == _native.PAGE_OPTIONS_VERSION


@pytest.mark.parametrize("kind", _native.ReadyKind)
def test_ready_kind_matches_the_c_header(kind: _native.ReadyKind) -> None:
    assert header_constant(f"MITTO_READY_{kind.name}") == kind


@pytest.mark.parametrize("event_type", _native.PageEventType)
def test_page_event_type_matches_the_c_header(event_type: _native.PageEventType) -> None:
    assert header_constant(f"MITTO_PAGE_EVENT_{event_type.name}") == event_type


@pytest.mark.parametrize("error", _native.PageErrorCode)
def test_page_error_matches_the_c_header(error: _native.PageErrorCode) -> None:
    assert header_constant(f"MITTO_PAGE_ERROR_{error.name}") == error


@pytest.mark.parametrize("level", _native.ConsoleLevel)
def test_console_level_matches_the_c_header(level: _native.ConsoleLevel) -> None:
    assert header_constant(f"MITTO_PAGE_CONSOLE_{level.name}") == level


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


@pytest.mark.usefixtures("native_library")
def test_native_library_exports_the_c_api_and_nothing_else() -> None:
    library = _native.find_library()
    assert library is not None
    # Except on Windows the one function that //base marks __declspec(dllexport), which no exports list can take away.
    # Windows looks up an import in the DLL it names, so it cannot clash with anything, and //base only looks for it in
    # the program that started the process.
    windows_extra: set[str] = {"GetHandleVerifier"} if sys.platform == "win32" else set()

    assert exported_names(library) == c_api() | windows_extra


@pytest.mark.skipif(sys.platform != "linux", reason="only Linux wheels have to avoid other system libraries")
@pytest.mark.usefixtures("native_library")
def test_native_library_needs_only_glibc_and_libgcc_on_linux() -> None:
    library = _native.find_library()
    assert library is not None
    ldd = subprocess.run(["ldd", library], check=True, capture_output=True, text=True).stdout
    assert {Path(line.split()[0]).name for line in ldd.splitlines()} <= LINUX_SYSTEM_LIBRARIES


@pytest.mark.skipif(sys.platform != "win32", reason="reads the import tables of a Windows DLL")
@pytest.mark.usefixtures("native_library")
def test_native_library_loads_only_windows_own_libraries_on_windows() -> None:
    library = _native.find_library()
    assert library is not None
    system = Path(os.environ["SYSTEMROOT"]) / "System32"

    imported = PeFile(library).imported_libraries()

    assert imported
    for name in imported:
        assert not C_RUNTIMES.match(name), name
        assert WINDOWS_API_SET.match(name) or (system / name).is_file(), name


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


def test_session_create_accepts_a_user_agent(native_library: ctypes.CDLL) -> None:
    error, handle = create_session(native_library, b'{"version": 1, "user_agent": "Mozilla/5.0"}')

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize("seed", sorted(_SETTINGS_FUZZ_SEEDS.glob("*.json")), ids=lambda path: path.stem)
def test_session_create_accepts_each_seed_of_the_fuzzer(native_library: ctypes.CDLL, seed: Path) -> None:
    # The parser stops at the first key it refuses, so a seed that is refused says nothing about those after it.
    error, handle = create_session(native_library, seed.read_bytes())

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize(
    "proxy",
    [
        "http://proxy.test",
        "http://proxy.test/",
        "http://proxy.test:8080",
        "HTTP://PROXY.TEST:8080",
        "http://user:password@proxy.test:8080",
        "http://user@proxy.test:8080",
        "http://:password@proxy.test:8080",
        "http://us%40er:p%3Ass@proxy.test:8080",
        "http://127.0.0.1:8080",
        "http://[::1]:8080",
        "https://proxy.test",
        "https://user:password@proxy.test:8443",
        "socks5://proxy.test",
        "socks5://proxy.test/",
        "socks5://proxy.test:1080",
        "SOCKS5://PROXY.TEST:1080",
        "socks5://127.0.0.1:80",
        "socks5://[::1]:1080",
        "socks5://user:password@proxy.test:1080",
        "socks5://user@proxy.test",
        "socks5://:password@proxy.test",
        "SOCKS5://user:password@proxy.test",
        "socks5://us%40er:p%FFss@proxy.test",
        f"socks5://{'u' * 255}:{'p' * 255}@proxy.test",
        f"socks5://{'%FF' * 255}@proxy.test",
    ],
)
def test_session_create_accepts_a_proxy(native_library: ctypes.CDLL, proxy: str) -> None:
    settings = f'{{"version": 1, "proxy": "{proxy}"}}'.encode()
    error, handle = create_session(native_library, settings)

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize("origin", ["example.com:443", "capture.localhost:8445", "127.0.0.1:8445", "*"])
def test_session_create_accepts_an_origin_to_force_quic_on(native_library: ctypes.CDLL, origin: str) -> None:
    settings = f'{{"version": 1, "origins_to_force_quic_on": ["{origin}"]}}'.encode()

    error, handle = create_session(native_library, settings)

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize("nameserver", ["127.0.0.1:53", "[::1]:5353"])
def test_session_create_accepts_a_dns_nameserver(native_library: ctypes.CDLL, nameserver: str) -> None:
    settings = f'{{"version": 1, "dns_nameserver": "{nameserver}"}}'.encode()

    error, handle = create_session(native_library, settings)

    assert error == 0
    native_library.mitto_session_destroy(handle)


@pytest.mark.parametrize(
    "rules",
    [
        "",
        "MAP capture.test 127.0.0.1",
        "MAP capture.test 127.0.0.1,MAP *.capture.test [::1]:443, EXCLUDE other.capture.test",
        "map CAPTURE.test 127.0.0.1",
    ],
)
def test_session_create_accepts_host_resolver_rules(native_library: ctypes.CDLL, rules: str) -> None:
    settings = f'{{"version": 1, "host_resolver_rules": "{rules}"}}'.encode()

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
        pytest.param(b'{"version": 1, "additional_trust_anchors": "x"}', id="anchors-is-a-string"),
        pytest.param(b'{"version": 1, "additional_trust_anchors": [1]}', id="anchor-is-a-number"),
        pytest.param(b'{"version": 1, "additional_trust_anchors": ["x"]}', id="anchor-has-no-pem-block"),
        pytest.param(b'{"version": 1, "accept_language": 1}', id="accept-language-is-a-number"),
        pytest.param(b'{"version": 1, "accept_language": "a\\r\\nb"}', id="crlf-in-accept-language"),
        pytest.param(b'{"version": 1, "accept_language": "a\\u0000b"}', id="nul-in-accept-language"),
        pytest.param(b'{"version": 1, "user_agent": 1}', id="user-agent-is-a-number"),
        pytest.param(b'{"version": 1, "user_agent": "a\\r\\nb"}', id="crlf-in-user-agent"),
        pytest.param(b'{"version": 1, "user_agent": "a\\u0000b"}', id="nul-in-user-agent"),
        pytest.param(b'{"version": 1, "proxy": 1}', id="proxy-is-a-number"),
        pytest.param(b'{"version": 1, "proxy": null}', id="proxy-is-null"),
        pytest.param(b'{"version": 1, "proxy": ""}', id="proxy-is-empty"),
        pytest.param(b'{"version": 1, "proxy": "proxy.test:8080"}', id="proxy-has-no-scheme"),
        pytest.param(b'{"version": 1, "proxy": "ftp://proxy.test"}', id="proxy-of-another-scheme"),
        pytest.param(b'{"version": 1, "proxy": "socks4://proxy.test"}', id="proxy-is-socks4"),
        pytest.param(b'{"version": 1, "proxy": "socks://proxy.test"}', id="proxy-is-socks"),
        pytest.param(b'{"version": 1, "proxy": "quic://proxy.test"}', id="proxy-is-quic"),
        pytest.param(b'{"version": 1, "proxy": "direct://"}', id="proxy-is-direct"),
        pytest.param(b'{"version": 1, "proxy": "http://"}', id="proxy-has-no-host"),
        pytest.param(b'{"version": 1, "proxy": "socks5://"}', id="socks5-proxy-has-no-host"),
        pytest.param(b'{"version": 1, "proxy": "socks5://:1080"}', id="socks5-proxy-has-only-a-port"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test:0"}', id="proxy-on-port-0"),
        pytest.param(b'{"version": 1, "proxy": "socks5://proxy.test:0"}', id="socks5-proxy-on-port-0"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test:65536"}', id="proxy-has-a-large-port"),
        pytest.param(b'{"version": 1, "proxy": "socks5://proxy.test:65536"}', id="socks5-proxy-has-a-large-port"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test:port"}', id="proxy-has-a-bad-port"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test/path"}', id="proxy-has-a-path"),
        pytest.param(b'{"version": 1, "proxy": "socks5://proxy.test/path"}', id="socks5-proxy-has-a-path"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test?query"}', id="proxy-has-a-query"),
        pytest.param(b'{"version": 1, "proxy": "http://proxy.test#fragment"}', id="proxy-has-a-fragment"),
        pytest.param(b'{"version": 1, "proxy": "socks5://a b:1080"}', id="socks5-proxy-has-a-space-in-its-host"),
        pytest.param(
            b'{"version": 1, "proxy": "socks5://%s@proxy.test"}' % (b"u" * 256), id="socks5-username-is-too-long"
        ),
        pytest.param(
            b'{"version": 1, "proxy": "socks5://%s@proxy.test"}' % (b"%FF" * 256),
            id="socks5-username-is-too-long-once-decoded",
        ),
        pytest.param(
            b'{"version": 1, "proxy": "socks5://user:%s@proxy.test"}' % (b"p" * 256), id="socks5-password-is-too-long"
        ),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": "a.test:443"}', id="origins-is-a-string"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": [1]}', id="origin-is-a-number"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": [""]}', id="origin-is-empty"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a.test"]}', id="origin-has-no-port"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a.test:65536"]}', id="origin-has-a-large-port"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["https://a.test:443"]}', id="origin-has-a-scheme"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["A.test:443"]}', id="origin-is-not-canonical"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["[::1]:443"]}', id="origin-is-ipv6"),
        pytest.param(b'{"version": 1, "origins_to_force_quic_on": ["a\\u0000.test:443"]}', id="nul-in-origin"),
        pytest.param(b'{"version": 1, "dns_nameserver": 53}', id="nameserver-is-a-number"),
        pytest.param(b'{"version": 1, "dns_nameserver": null}', id="nameserver-is-null"),
        pytest.param(b'{"version": 1, "dns_nameserver": ""}', id="nameserver-is-empty"),
        pytest.param(b'{"version": 1, "dns_nameserver": "127.0.0.1"}', id="nameserver-has-no-port"),
        pytest.param(b'{"version": 1, "dns_nameserver": "127.0.0.1:0"}', id="nameserver-has-port-zero"),
        pytest.param(b'{"version": 1, "dns_nameserver": "127.0.0.1:65536"}', id="nameserver-has-a-large-port"),
        pytest.param(b'{"version": 1, "dns_nameserver": "dns.test:53"}', id="nameserver-is-a-name"),
        pytest.param(b'{"version": 1, "dns_nameserver": "::1:53"}', id="nameserver-ipv6-without-brackets"),
        pytest.param(b'{"version": 1, "refused_connects": -1}', id="refused-connects-are-negative"),
        pytest.param(b'{"version": 1, "refused_connects": 1.5}', id="refused-connects-are-a-fraction"),
        pytest.param(b'{"version": 1, "refused_connects": "2"}', id="refused-connects-are-a-string"),
        pytest.param(b'{"version": 1, "refused_connects": null}', id="refused-connects-are-null"),
        pytest.param(b'{"version": 1, "host_resolver_rules": 1}', id="rules-are-a-number"),
        pytest.param(b'{"version": 1, "host_resolver_rules": ["MAP a.test 127.0.0.1"]}', id="rules-are-a-list"),
        pytest.param(b'{"version": 1, "host_resolver_rules": "nonsense"}', id="rule-is-nonsense"),
        pytest.param(b'{"version": 1, "host_resolver_rules": "MAP a.test"}', id="rule-has-no-address"),
        pytest.param(b'{"version": 1, "host_resolver_rules": "MAP a.test b c"}', id="rule-has-too-much"),
        pytest.param(b'{"version": 1, "host_resolver_rules": "MAP a.test b:port"}', id="rule-has-a-bad-port"),
        pytest.param(b'{"version": 1, "host_resolver_rules": "MAP a.test 127.0.0.1,"}', id="rule-is-empty"),
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


@pytest.mark.skipif(sys.platform != "win32", reason="a Session has a file descriptor on macOS and Linux")
def test_event_fd_refuses_on_windows(native_library: ctypes.CDLL, session_handle: Any) -> None:
    fd = ctypes.c_int32(0)

    assert native_library.mitto_session_event_fd(session_handle, ctypes.byref(fd)) == ERR_NOT_IMPLEMENTED
    assert fd.value == -1


def test_event_fd_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    fd = ctypes.c_int32(1)

    assert native_library.mitto_session_event_fd(None, ctypes.byref(fd)) == ERR_INVALID_ARGUMENT
    assert fd.value == -1
    assert native_library.mitto_session_event_fd(session_handle, None) == ERR_INVALID_ARGUMENT


def test_list_cookies_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    cookies = ctypes.POINTER(ctypes.c_char)(ctypes.c_char(b"x"))
    size = ctypes.c_size_t(1)

    assert (
        native_library.mitto_session_list_cookies(None, ctypes.byref(cookies), ctypes.byref(size))
        == ERR_INVALID_ARGUMENT
    )
    assert not cookies
    assert size.value == 0
    assert native_library.mitto_session_list_cookies(session_handle, None, ctypes.byref(size)) == ERR_INVALID_ARGUMENT
    assert (
        native_library.mitto_session_list_cookies(session_handle, ctypes.byref(cookies), None) == ERR_INVALID_ARGUMENT
    )


def test_set_cookies_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    cookies = f'{{"version": {_native.COOKIES_VERSION}, "cookies": []}}'.encode()

    assert native_library.mitto_session_set_cookies(None, cookies, len(cookies)) == ERR_INVALID_ARGUMENT
    assert native_library.mitto_session_set_cookies(session_handle, None, 0) == ERR_INVALID_ARGUMENT
    assert native_library.mitto_session_set_cookies(session_handle, cookies, len(cookies)) == 0


def test_delete_cookies_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    cookie_filter = f'{{"version": {_native.COOKIES_VERSION}}}'.encode()
    deleted = ctypes.c_uint32(1)

    assert (
        native_library.mitto_session_delete_cookies(None, cookie_filter, len(cookie_filter), ctypes.byref(deleted))
        == ERR_INVALID_ARGUMENT
    )
    assert deleted.value == 0
    assert (
        native_library.mitto_session_delete_cookies(session_handle, None, 0, ctypes.byref(deleted))
        == ERR_INVALID_ARGUMENT
    )
    assert (
        native_library.mitto_session_delete_cookies(session_handle, cookie_filter, len(cookie_filter), None)
        == ERR_INVALID_ARGUMENT
    )


def test_cookies_free_takes_null(native_library: ctypes.CDLL) -> None:
    native_library.mitto_cookies_free(None)


def test_next_ready_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    kind = ctypes.c_int32(7)
    handle = ctypes.c_void_p(1)

    assert (
        native_library.mitto_session_next_ready(None, ctypes.byref(kind), ctypes.byref(handle)) == ERR_INVALID_ARGUMENT
    )
    assert (kind.value, handle.value) == (0, None)
    assert native_library.mitto_session_next_ready(session_handle, None, ctypes.byref(handle)) == ERR_INVALID_ARGUMENT
    assert native_library.mitto_session_next_ready(session_handle, ctypes.byref(kind), None) == ERR_INVALID_ARGUMENT


def test_next_ready_stores_nothing_when_nothing_is_waiting(native_library: ctypes.CDLL, session_handle: Any) -> None:
    kind = ctypes.c_int32(7)
    handle = ctypes.c_void_p(1)

    assert native_library.mitto_session_next_ready(session_handle, ctypes.byref(kind), ctypes.byref(handle)) == (
        ERR_IO_PENDING
    )
    assert (kind.value, handle.value) == (0, None)


def test_wait_for_event_refuses_a_null_session_and_end_waits_takes_one(native_library: ctypes.CDLL) -> None:
    assert native_library.mitto_session_wait_for_event(None, 0) == ERR_INVALID_ARGUMENT
    native_library.mitto_session_end_waits(None)


def test_wait_for_event_says_when_nothing_came_in_time(native_library: ctypes.CDLL, session_handle: Any) -> None:
    assert native_library.mitto_session_wait_for_event(session_handle, 0) == ERR_IO_PENDING
    assert native_library.mitto_session_wait_for_event(session_handle, 50) == ERR_IO_PENDING


def test_end_waits_ends_every_wait_from_then_on(native_library: ctypes.CDLL, session_handle: Any) -> None:
    waited: list[int] = []
    waiting = threading.Thread(
        target=lambda: waited.append(native_library.mitto_session_wait_for_event(session_handle, -1))
    )
    waiting.start()
    waiting.join(0.05)
    assert waiting.is_alive()

    native_library.mitto_session_end_waits(session_handle)

    waiting.join(EVENT_TIMEOUT_MS / 1000)
    assert waited == [ERR_ABORTED]
    assert native_library.mitto_session_wait_for_event(session_handle, -1) == ERR_ABORTED


def test_destroying_a_session_ends_a_wait_and_waits_for_it(native_library: ctypes.CDLL) -> None:
    error, handle = create_session(native_library, NO_SETTINGS)
    assert error == 0
    waited: list[int] = []
    waiting = threading.Thread(target=lambda: waited.append(native_library.mitto_session_wait_for_event(handle, -1)))
    waiting.start()
    # Long enough for the thread to be in the call, since one that started after the Session had gone would use it.
    waiting.join(0.2)
    assert waiting.is_alive()

    native_library.mitto_session_destroy(handle)

    waiting.join(EVENT_TIMEOUT_MS / 1000)
    assert waited == [ERR_ABORTED]


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
        session_handle, server_url.encode(), None, 0, NO_OPTIONS, len(NO_OPTIONS), None, 0, None
    )

    assert error == ERR_INVALID_ARGUMENT


def test_request_start_refuses_a_size_for_a_body_that_is_null(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), body=None, body_size=1)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


def test_request_start_reads_the_body_by_its_size(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    error, handle = start_request(native_library, session_handle, server_url.encode(), body=b"a\0body", body_size=3)

    assert error == 0
    native_library.mitto_request_release(handle)


def test_request_start_reads_the_options_by_their_size(
    native_library: ctypes.CDLL, session_handle: Any, server_url: str
) -> None:
    handle = ctypes.POINTER(_native.MittoRequest)()

    error = native_library.mitto_request_start(
        session_handle,
        server_url.encode(),
        None,
        0,
        NO_OPTIONS + b"not json",
        len(NO_OPTIONS),
        None,
        0,
        ctypes.byref(handle),
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
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "navigate", "destination": "document", "credentials_mode": "include"}}',
            id="fetch-with-what-it-needs",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "same-origin", '
            b'"has_user_activation": true, "initiator": "https://a.test/page", "fixed_headers": ["Accept", "a b"]}}',
            id="fetch-with-everything",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "no-cors", "destination": "script", "credentials_mode": "include", '
            b'"initiator": "data:text/plain,hello"}}',
            id="fetch-from-an-opaque-origin",
        ),
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
        pytest.param(b'{"version": 1, "fetch": null}', id="fetch-is-null"),
        pytest.param(b'{"version": 1, "fetch": {}}', id="fetch-is-empty"),
        pytest.param(b'{"version": 1, "fetch": {"mode": "cors"}}', id="fetch-without-the-rest"),
        pytest.param(
            b'{"version": 1, "fetch": {"destination": "empty", "credentials_mode": "include"}}',
            id="fetch-without-a-mode",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "credentials_mode": "include"}}',
            id="fetch-without-a-destination",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty"}}',
            id="fetch-without-a-credentials-mode",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "same-origin", "destination": "empty", "credentials_mode": "include"}}',
            id="another-mode",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "em pty", "credentials_mode": "include"}}',
            id="destination-is-no-token",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "", "credentials_mode": "include"}}',
            id="destination-is-empty",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "omit"}}',
            id="another-credentials-mode",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"has_user_activation": 1}}',
            id="has-user-activation-is-a-number",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"initiator": "not a url"}}',
            id="initiator-is-not-a-url",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"initiator": null}}',
            id="initiator-is-null",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"fixed_headers": "accept"}}',
            id="fixed-headers-is-a-string",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"fixed_headers": [1]}}',
            id="fixed-headers-holds-a-number",
        ),
        pytest.param(
            b'{"version": 1, "fetch": {"mode": "cors", "destination": "empty", "credentials_mode": "include", '
            b'"unknown": true}}',
            id="unknown-key-in-fetch",
        ),
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


# What a Page starts with, as a Session's Machine and Persona say it.
PAGE_START: dict[str, Any] = {
    "time_zone": "America/New_York",
    "locale": "en-US",
    "processors": 8,
    "touch_points": 0,
    "work_area_x": 0,
    "work_area_y": 0,
    "work_area_width": 1920,
    "work_area_height": 1032,
    "window_state": "maximised",
    "window_x": 0,
    "window_y": 0,
    "window_width": 1920,
    "window_height": 1032,
    "contrast_theme": "none",
    "inverted_colors": "none",
}


def blank_page_options(**start: Any) -> bytes:
    """Return the options of a Page that stays at about:blank, with each of `start` in place in what it starts with."""
    return json.dumps({"version": _native.PAGE_OPTIONS_VERSION, "start": {**PAGE_START, **start}}).encode()


BLANK_PAGE_OPTIONS = blank_page_options()
ERR_MSG_TOO_BIG = -142
# The longest script that one message to a Page carries.
MAX_SCRIPT_SIZE = 16 * 1024 * 1024 - 8


def open_page(library: ctypes.CDLL, session: Any, options: bytes | None) -> tuple[int, Any]:
    """Call mitto_page_open and return its result and the handle it stored."""
    # Not null at first, to show that a failed call stores null.
    handle = ctypes.pointer(_native.MittoPage())
    error: int = library.mitto_page_open(session, options, len(options or b""), ctypes.byref(handle))
    return error, handle


def next_page_event(library: ctypes.CDLL, page: Any) -> tuple[int, int, int, str, str]:
    """Take the Page's next event, and return its type, error, call, JSON and message."""
    event = _native.MittoPageEvent()
    assert library.mitto_page_next_event(page, EVENT_TIMEOUT_MS, ctypes.byref(event)) == 0
    try:
        json_text = ctypes.string_at(event.json, event.json_size).decode() if event.json else ""
        return event.type, event.error, event.call, json_text, page_event_message(event)
    finally:
        library.mitto_page_event_free(ctypes.byref(event))
        assert not event.json
        assert not event.message


def page_event_message(event: _native.MittoPageEvent) -> str:
    """Return the message of a Page's event, by its size, which counts any NUL character in it."""
    return ctypes.string_at(event.message, event.message_size).decode() if event.message else ""


def next_page_report(library: ctypes.CDLL, page: Any) -> tuple[int, int, int, str]:
    """Take the Page's next event, and return its type, error, level and message."""
    event = _native.MittoPageEvent()
    assert library.mitto_page_next_event(page, EVENT_TIMEOUT_MS, ctypes.byref(event)) == 0
    try:
        return event.type, event.error, event.level, page_event_message(event)
    finally:
        library.mitto_page_event_free(ctypes.byref(event))


def evaluate(library: ctypes.CDLL, page: Any, script: bytes) -> tuple[int, int]:
    """Call mitto_page_evaluate and return its result and the call it stored."""
    call = ctypes.c_uint64(99)
    error: int = library.mitto_page_evaluate(page, script, len(script), ctypes.byref(call))
    return error, call.value


@pytest.fixture
def page_program(native_library: ctypes.CDLL) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name("mitto_page.exe" if sys.platform == "win32" else "mitto_page").is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_handle(native_library: ctypes.CDLL, session_handle: Any, page_program: None) -> Iterator[Any]:
    """Open a Page on a real Session, or skip if no Page program lies beside the library."""
    error, handle = open_page(native_library, session_handle, BLANK_PAGE_OPTIONS)
    assert error == 0
    yield handle
    native_library.mitto_page_close(handle)


def _page_options(**changes: Any) -> bytes:
    """Return the options of a Page that navigates, with each of `changes` in place: None leaves a key out."""
    template: dict[str, Any] = {
        "headers": [["Accept", "*/*"]],
        "options": {
            "version": _native.REQUEST_OPTIONS_VERSION,
            "fetch": {"mode": "navigate", "destination": "document", "credentials_mode": "include"},
        },
    }
    options: dict[str, Any] = {
        "version": _native.PAGE_OPTIONS_VERSION,
        "start": PAGE_START,
        "url": "http://127.0.0.1:1/",
        "requests": {"navigation": template, "script": template},
    }
    for key, value in changes.items():
        if value is None:
            options.pop(key)
        else:
            options[key] = value
    return json.dumps(options).encode()


def _template(headers: Any = (("Accept", "*/*"),), **fetch: Any) -> dict[str, Any]:
    return {
        "headers": [list(header) for header in headers],
        "options": {
            "version": _native.REQUEST_OPTIONS_VERSION,
            "fetch": {"mode": "no-cors", "destination": "script", "credentials_mode": "include", **fetch},
        },
    }


@pytest.mark.parametrize(
    "options",
    [
        pytest.param(b"", id="empty"),
        pytest.param(b"{}", id="no-version"),
        pytest.param(_page_options(version=1), id="another-version"),
        pytest.param(_page_options(referrer="https://example.com/"), id="unknown-key"),
        pytest.param(_page_options(requests=None), id="url-without-a-navigation"),
        pytest.param(_page_options(start=None), id="no-start"),
        pytest.param(_page_options(start=[]), id="start-not-an-object"),
        pytest.param(_page_options(url=7), id="url-not-a-string"),
        pytest.param(_page_options(requests=[]), id="requests-not-an-object"),
        pytest.param(_page_options(requests={"image": _template()}), id="unknown-kind"),
        pytest.param(_page_options(requests={"navigation": _template(initiator="https://a.test/")}), id="initiator"),
        pytest.param(_page_options(requests={"navigation": _template(fixed_headers=["accept"])}), id="fixed-headers"),
        pytest.param(
            _page_options(requests={"navigation": _template(headers=[["Accept", "\u0100"]])}), id="not-latin1"
        ),
        pytest.param(_page_options(requests={"navigation": _template(headers=[["Referer", "x"]])}), id="referer"),
        pytest.param(_page_options(requests={"navigation": _template(headers=[["A", "1"], ["a", "2"]])}), id="twice"),
        pytest.param(_page_options(requests={"navigation": {"headers": [], "options": {"version": 1}}}), id="no-fetch"),
        pytest.param(
            _page_options(
                requests={"navigation": {**_template(), "options": {**_template()["options"], "total_timeout_ms": 5}}}
            ),
            id="timer",
        ),
        pytest.param(_page_options(requests={"navigation": {"headers": []}}), id="no-options"),
        pytest.param(_page_options(time_zone="UTC"), id="time-zone-outside-the-start"),
        pytest.param(blank_page_options(time_zone=9), id="time-zone-not-a-string"),
        pytest.param(blank_page_options(time_zone=""), id="empty-time-zone"),
        pytest.param(blank_page_options(time_zone="Europe/Paris\u0000"), id="time-zone-with-nul"),
        pytest.param(blank_page_options(locale="de DE"), id="locale-with-space"),
        pytest.param(blank_page_options(locale="a" * 65), id="locale-too-long"),
        pytest.param(blank_page_options(screen_width=1920), id="unknown-key-in-the-start"),
        pytest.param(
            json.dumps({"version": _native.PAGE_OPTIONS_VERSION, "start": {**PAGE_START, "processors": None}}).encode(),
            id="processors-null",
        ),
        pytest.param(
            json.dumps(
                {
                    "version": _native.PAGE_OPTIONS_VERSION,
                    "start": {name: value for name, value in PAGE_START.items() if name != "touch_points"},
                }
            ).encode(),
            id="no-touch-points",
        ),
        pytest.param(blank_page_options(processors=0), id="no-processors"),
        pytest.param(blank_page_options(processors=65), id="too-many-processors"),
        pytest.param(blank_page_options(processors=8.5), id="processors-not-whole"),
        pytest.param(blank_page_options(processors="8"), id="processors-a-string"),
        pytest.param(blank_page_options(touch_points=-1), id="touch-points-below-0"),
        pytest.param(blank_page_options(touch_points=257), id="too-many-touch-points"),
        pytest.param(blank_page_options(work_area_x=-1), id="work-area-left-of-the-display"),
        pytest.param(blank_page_options(work_area_height=0), id="work-area-of-no-height"),
        pytest.param(blank_page_options(window_width=0), id="window-of-no-width"),
        pytest.param(blank_page_options(window_x=2**31), id="window-place-beyond-32-bits"),
        pytest.param(blank_page_options(window_state="first"), id="unknown-window-state"),
        pytest.param(blank_page_options(contrast_theme="high-contrast"), id="unknown-contrast-theme"),
        pytest.param(blank_page_options(inverted_colors="reduce"), id="unknown-inverted-colors"),
    ],
)
def test_page_open_refuses_invalid_options(native_library: ctypes.CDLL, session_handle: Any, options: bytes) -> None:
    error, handle = open_page(native_library, session_handle, options)

    assert error == ERR_INVALID_ARGUMENT
    assert not handle


@pytest.mark.parametrize(
    "start",
    [
        {},
        {"window_state": "normal", "window_x": -8, "window_y": 10, "window_width": 945, "window_height": 1012},
        {"work_area_x": 40, "work_area_width": 1880},
        {"processors": 64, "touch_points": 256},
        {"processors": 1, "touch_points": 10},
        {"contrast_theme": "night-sky", "inverted_colors": "inverted"},
        {"contrast_theme": "high-contrast-white"},
        {"time_zone": "Etc/GMT+12", "locale": "zh-Hans-CN"},
    ],
    ids=str,
)
def test_page_open_takes_what_a_page_starts_with(
    native_library: ctypes.CDLL, session_handle: Any, page_program: None, start: dict[str, Any]
) -> None:
    error, handle = open_page(native_library, session_handle, blank_page_options(**start))

    assert error == 0
    native_library.mitto_page_close(handle)


def test_page_open_takes_a_url_and_what_each_kind_sends(
    native_library: ctypes.CDLL, session_handle: Any, page_program: None
) -> None:
    # The navigation fails, since Chrome never connects to port 1, which its event says.
    error, handle = open_page(native_library, session_handle, _page_options())
    assert error == 0
    try:
        event = _native.MittoPageEvent()
        assert native_library.mitto_page_next_event(handle, 30_000, ctypes.byref(event)) == 0
        try:
            assert (event.type, event.error, event.status, event.net_error) == (
                _native.PageEventType.NAVIGATED,
                0,
                0,
                ERR_UNSAFE_PORT,
            )
            assert ctypes.string_at(event.url, event.url_size) == b"http://127.0.0.1:1/"
        finally:
            native_library.mitto_page_event_free(ctypes.byref(event))
    finally:
        native_library.mitto_page_close(handle)


def test_page_open_refuses_null_arguments(native_library: ctypes.CDLL, session_handle: Any) -> None:
    error, handle = open_page(native_library, None, BLANK_PAGE_OPTIONS)
    assert (error, bool(handle)) == (ERR_INVALID_ARGUMENT, False)
    error, handle = open_page(native_library, session_handle, None)
    assert (error, bool(handle)) == (ERR_INVALID_ARGUMENT, False)
    assert native_library.mitto_page_open(session_handle, BLANK_PAGE_OPTIONS, len(BLANK_PAGE_OPTIONS), None) == (
        ERR_INVALID_ARGUMENT
    )


def test_null_pages_are_refused_or_accepted_where_nothing_needs_doing(native_library: ctypes.CDLL) -> None:
    process_id = ctypes.c_int64(7)
    assert native_library.mitto_page_process_id(None, ctypes.byref(process_id)) == ERR_INVALID_ARGUMENT
    assert process_id.value == 0
    assert evaluate(native_library, None, b"1") == (ERR_INVALID_ARGUMENT, 0)
    event = _native.MittoPageEvent(type=7)
    assert native_library.mitto_page_next_event(None, 0, ctypes.byref(event)) == ERR_INVALID_ARGUMENT
    assert event.type == 0
    native_library.mitto_page_event_free(None)
    native_library.mitto_page_end_waits(None)
    native_library.mitto_page_close(None)


def test_a_page_gives_each_result_as_json_with_its_call(native_library: ctypes.CDLL, page_handle: Any) -> None:
    first = evaluate(native_library, page_handle, b"var a = {b: [1, 'two', null]}; a")
    second = evaluate(native_library, page_handle, b"a.b.length")
    third = evaluate(native_library, page_handle, b"undefined")

    assert [first[0], second[0], third[0]] == [0, 0, 0]
    assert first[1] < second[1] < third[1]
    evaluated = _native.PageEventType.EVALUATED
    assert next_page_event(native_library, page_handle) == (evaluated, 0, first[1], '{"b":[1,"two",null]}', "")
    assert next_page_event(native_library, page_handle) == (evaluated, 0, second[1], "3", "")
    assert next_page_event(native_library, page_handle) == (evaluated, 0, third[1], "", "")


def test_a_pages_context_has_the_interfaces_that_the_runtime_implements(
    native_library: ctypes.CDLL, page_handle: Any
) -> None:
    # The bindings: tests/test_page_bindings.py compares them with Chrome through the Page program's script argument.
    script = b"const t = new EventTarget(); let n = 0; t.addEventListener('x', () => n++); [t.dispatchEvent(new Event('x')), n]"
    _, call = evaluate(native_library, page_handle, script)

    assert next_page_event(native_library, page_handle)[2:4] == (call, "[true,1]")


def test_a_script_that_throws_fails_its_call_and_the_page_goes_on(
    native_library: ctypes.CDLL, page_handle: Any
) -> None:
    _, call = evaluate(native_library, page_handle, b"null.x")

    event_type, error, answered, json_text, message = next_page_event(native_library, page_handle)

    assert (event_type, error, answered, json_text) == (
        _native.PageEventType.EVALUATED,
        _native.PageErrorCode.SCRIPT,
        call,
        "",
    )
    assert message.startswith("Uncaught TypeError: ")
    _, call = evaluate(native_library, page_handle, b"6 * 7")
    assert next_page_event(native_library, page_handle)[2:4] == (call, "42")


def test_page_evaluate_refuses_a_script_that_is_not_utf8_or_too_long(
    native_library: ctypes.CDLL, page_handle: Any
) -> None:
    assert evaluate(native_library, page_handle, b"'\xff'") == (ERR_INVALID_ARGUMENT, 0)
    assert evaluate(native_library, page_handle, b" " * (MAX_SCRIPT_SIZE + 1)) == (ERR_MSG_TOO_BIG, 0)
    assert native_library.mitto_page_evaluate(page_handle, b"1", 1, None) == ERR_INVALID_ARGUMENT


def test_page_next_event_says_when_no_event_is_waiting(native_library: ctypes.CDLL, page_handle: Any) -> None:
    event = _native.MittoPageEvent()

    assert native_library.mitto_page_next_event(page_handle, 0, ctypes.byref(event)) == ERR_IO_PENDING
    assert event.type == 0


def test_a_page_has_a_process(native_library: ctypes.CDLL, page_handle: Any) -> None:
    process_id = ctypes.c_int64()

    assert native_library.mitto_page_process_id(page_handle, ctypes.byref(process_id)) == 0
    assert process_id.value > 0
    assert process_id.value != os.getpid()


def test_a_page_starts_with_the_time_zone_and_locale_of_its_options(
    native_library: ctypes.CDLL, session_handle: Any, page_handle: Any
) -> None:
    options = blank_page_options(time_zone="America/Sao_Paulo", locale="pt-BR")
    error, page = open_page(native_library, session_handle, options)
    assert error == 0
    try:
        script = (
            b"Intl.DateTimeFormat().resolvedOptions().timeZone + ' ' + Intl.NumberFormat().resolvedOptions().locale"
        )
        _, call = evaluate(native_library, page, script)
        assert next_page_event(native_library, page)[1:4] == (0, call, '"America/Sao_Paulo pt-BR"')
    finally:
        native_library.mitto_page_close(page)


def test_a_page_answers_a_call_whose_promise_settles_once_it_has(native_library: ctypes.CDLL, page_handle: Any) -> None:
    _, waiting = evaluate(native_library, page_handle, b"new Promise(resolve => globalThis.settle = resolve)")
    _, settled = evaluate(native_library, page_handle, b"Promise.resolve(2)")
    _, settling = evaluate(native_library, page_handle, b"settle('later'); 3")

    evaluated = _native.PageEventType.EVALUATED
    assert next_page_event(native_library, page_handle) == (evaluated, 0, settled, "2", "")
    assert next_page_event(native_library, page_handle) == (evaluated, 0, waiting, '"later"', "")
    assert next_page_event(native_library, page_handle) == (evaluated, 0, settling, "3", "")


def test_a_page_reports_console_messages_and_uncaught_errors_before_its_result(
    native_library: ctypes.CDLL, page_handle: Any
) -> None:
    script = b"console.warn('careful', 1); Promise.reject(new Error('lost')); console.log('\\u00e9'); 0"
    _, call = evaluate(native_library, page_handle, script)

    console = _native.PageEventType.CONSOLE
    assert next_page_report(native_library, page_handle) == (console, 0, _native.ConsoleLevel.WARNING, "careful 1")
    assert next_page_report(native_library, page_handle) == (console, 0, _native.ConsoleLevel.LOG, "\u00e9")
    assert next_page_report(native_library, page_handle) == (
        _native.PageEventType.UNCAUGHT_ERROR,
        _native.PageErrorCode.SCRIPT,
        0,
        "Uncaught (in promise) Error: lost",
    )
    assert next_page_event(native_library, page_handle)[:4] == (_native.PageEventType.EVALUATED, 0, call, "0")


def test_the_message_of_a_page_event_keeps_a_nul_character(native_library: ctypes.CDLL, page_handle: Any) -> None:
    _, call = evaluate(native_library, page_handle, b"console.log('a\\0b'); throw 'c\\0d'")

    assert next_page_report(native_library, page_handle) == (
        _native.PageEventType.CONSOLE,
        0,
        _native.ConsoleLevel.LOG,
        "a\0b",
    )
    assert next_page_event(native_library, page_handle) == (
        _native.PageEventType.EVALUATED,
        _native.PageErrorCode.SCRIPT,
        call,
        "",
        "Uncaught c\0d",
    )


def test_page_end_waits_ends_a_wait_on_another_thread_and_each_later_one(
    native_library: ctypes.CDLL, page_handle: Any
) -> None:
    def wait() -> int:
        event = _native.MittoPageEvent()
        return native_library.mitto_page_next_event(page_handle, -1, ctypes.byref(event))

    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        waiting = executor.submit(wait)
        time.sleep(0.1)
        assert not waiting.done()
        native_library.mitto_page_end_waits(page_handle)
        assert waiting.result(timeout=EVENT_TIMEOUT_MS / 1000) == ERR_IO_PENDING

    assert wait() == ERR_IO_PENDING
    _, call = evaluate(native_library, page_handle, b"1")
    deadline = time.monotonic() + EVENT_TIMEOUT_MS / 1000
    event = _native.MittoPageEvent()
    while native_library.mitto_page_next_event(page_handle, -1, ctypes.byref(event)) == ERR_IO_PENDING:
        assert time.monotonic() < deadline
        time.sleep(0.01)
    assert (event.type, event.call) == (_native.PageEventType.EVALUATED, call)
    native_library.mitto_page_event_free(ctypes.byref(event))


def test_a_page_drops_reports_that_wait_untaken_past_the_limit_and_says_how_many(
    native_library: ctypes.CDLL, session_handle: Any, page_handle: Any
) -> None:
    # Only what arrives after this counts in the Session's list of what is ready.
    native_library.mitto_session_wait_for_event(session_handle, 0)
    _, call = evaluate(native_library, page_handle, b"for (let i = 0; i < 1500; i++) console.log(i); 'done'")

    # The list names the Page once for each event that it holds: 1000 console messages and the result.
    wait_until_named(native_library, session_handle, 1001)

    console = _native.PageEventType.CONSOLE
    log = _native.ConsoleLevel.LOG
    for number in range(1000):
        assert next_page_report(native_library, page_handle) == (console, 0, log, str(number))
    assert next_page_event(native_library, page_handle)[:4] == (_native.PageEventType.EVALUATED, 0, call, '"done"')
    _, call = evaluate(native_library, page_handle, b"console.log('after')")
    assert next_page_report(native_library, page_handle) == (
        console,
        0,
        _native.ConsoleLevel.WARNING,
        "the library dropped 500 console messages and uncaught errors of this Page, because too many waited untaken",
    )
    assert next_page_report(native_library, page_handle) == (console, 0, log, "after")
    assert next_page_event(native_library, page_handle)[:3] == (_native.PageEventType.EVALUATED, 0, call)


def test_the_note_of_dropped_reports_counts_toward_the_limit_on_their_text(
    native_library: ctypes.CDLL, session_handle: Any, page_handle: Any
) -> None:
    # Two of the longest console messages fill the 32 MiB that may wait, but for 2 bytes.
    longest = 16 * 1024 * 1024 - 1
    native_library.mitto_session_wait_for_event(session_handle, 0)
    script = f"const longest = 'x'.repeat({longest}); for (let i = 0; i < 3; i++) console.log(longest); 1"
    _, first = evaluate(native_library, page_handle, script.encode())
    wait_until_named(native_library, session_handle, 3)
    report = next_page_report(native_library, page_handle)
    assert (report[:3], len(report[3])) == ((_native.PageEventType.CONSOLE, 0, _native.ConsoleLevel.LOG), longest)

    # With the one taken, another fits by itself, but not with the note that says the third was dropped.
    _, second = evaluate(native_library, page_handle, b"console.log(longest); 2")
    wait_until_named(native_library, session_handle, 1)

    console = _native.PageEventType.CONSOLE
    report = next_page_report(native_library, page_handle)
    assert (report[0], len(report[3])) == (console, longest)
    assert next_page_event(native_library, page_handle)[2:4] == (first, "1")
    assert next_page_event(native_library, page_handle)[2:4] == (second, "2")
    _, third = evaluate(native_library, page_handle, b"console.log('small')")
    assert next_page_report(native_library, page_handle) == (
        console,
        0,
        _native.ConsoleLevel.WARNING,
        "the library dropped 2 console messages and uncaught errors of this Page, because too many waited untaken",
    )
    assert next_page_report(native_library, page_handle) == (console, 0, _native.ConsoleLevel.LOG, "small")
    assert next_page_event(native_library, page_handle)[2] == third


def wait_until_named(library: ctypes.CDLL, session: Any, count: int) -> None:
    """Wait until the Session's list of what is ready has named `count` more events."""
    named = 0
    kind = ctypes.c_int32()
    handle = ctypes.c_void_p()
    deadline = time.monotonic() + EVENT_TIMEOUT_MS / 1000
    while named < count:
        if library.mitto_session_next_ready(session, ctypes.byref(kind), ctypes.byref(handle)) == 0:
            named += 1
        else:
            assert time.monotonic() < deadline
            time.sleep(0.01)


def test_a_page_that_refuses_its_time_zone_ends_and_says_why(
    native_library: ctypes.CDLL, session_handle: Any, page_handle: Any
) -> None:
    error, page = open_page(native_library, session_handle, blank_page_options(time_zone="Mars/Olympus_Mons"))
    assert error == 0
    try:
        _, call = evaluate(native_library, page, b"1")
        why = 'the Page refused a message from the library: the time zone "Mars/Olympus_Mons" is unknown'
        refused = _native.PageErrorCode.REFUSED
        assert next_page_event(native_library, page) == (_native.PageEventType.EVALUATED, refused, call, "", why)
        assert next_page_event(native_library, page) == (_native.PageEventType.ENDED, refused, 0, "", why)
    finally:
        native_library.mitto_page_close(page)
