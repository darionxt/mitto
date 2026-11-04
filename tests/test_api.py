"""Tests for the public API.

Tests that take the native_library fixture go through the real native core to a local server, so they need no network
connection, and are skipped when this machine has no native library.
"""

import http.server
import os
import pickle
import subprocess
import sys
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native

ERR_INVALID_URL = -300

# The NUL shows that the body is read by its size, not as a C string.
BODY = b"a body with a \x00 inside"

# Written to the socket as they are, so that the expected headers do not depend on what http.server adds. The value
# with a byte above 0x7f is not valid UTF-8, and 0x85 is a line break to str.splitlines().
RAW_HEADERS = (
    b"Set-Cookie: a=1\r\n"
    b"X-Colon: one: two\r\n"
    b"X-Empty:\r\n"
    b"X-Latin-1: caf\xe9 \x85 end\r\n"
    b"Set-Cookie: b=2\r\n"
    b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
)


class RawResponseHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        status_line = b"HTTP/1.0 404 Not Found" if self.path == "/missing" else b"HTTP/1.0 203 Non-Authoritative"
        self.wfile.write(status_line + b"\r\n" + RAW_HEADERS + b"\r\n" + BODY)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server_url() -> Iterator[str]:
    """Run a local HTTP server and return its URL."""
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), RawResponseHandler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}/"
        server.shutdown()


@pytest.mark.usefixtures("native_library")
def test_get_returns_the_status(server_url: str) -> None:
    assert mitto.get(server_url).status == 203


@pytest.mark.usefixtures("native_library")
def test_get_returns_headers_in_the_order_received(server_url: str) -> None:
    assert mitto.get(server_url).headers == (
        ("Set-Cookie", "a=1"),
        ("X-Colon", "one: two"),
        ("X-Empty", ""),
        ("X-Latin-1", "caf\xe9 \x85 end"),
        ("Set-Cookie", "b=2"),
        ("Content-Length", str(len(BODY))),
    )


@pytest.mark.usefixtures("native_library")
def test_get_returns_the_body_as_bytes(server_url: str) -> None:
    assert mitto.get(server_url).body == BODY


@pytest.mark.usefixtures("native_library")
def test_get_returns_an_error_status_as_a_response(server_url: str) -> None:
    assert mitto.get(f"{server_url}missing").status == 404


@pytest.mark.usefixtures("native_library")
def test_get_raises_request_error_with_the_net_error() -> None:
    with pytest.raises(mitto.RequestError) as caught:
        mitto.get("not a url")

    assert caught.value.url == "not a url"
    assert caught.value.net_error == ERR_INVALID_URL
    assert str(ERR_INVALID_URL) in str(caught.value)


def test_request_error_survives_pickling() -> None:
    restored = pickle.loads(pickle.dumps(mitto.RequestError("http://example.test/", ERR_INVALID_URL)))

    assert restored.url == "http://example.test/"
    assert restored.net_error == ERR_INVALID_URL


def test_get_rejects_a_url_with_a_nul_before_loading_the_library(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    # The C API would stop reading at the NUL and fetch a different URL.
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(ValueError, match="NUL"):
        mitto.get("http://127.0.0.1/\0.example.test/")


def test_get_without_a_library_raises_native_library_error(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv(_native.LIBRARY_ENV_VAR, str(tmp_path / "missing"))

    with pytest.raises(mitto.NativeLibraryError, match=_native.LIBRARY_ENV_VAR):
        mitto.get("http://127.0.0.1/")


def test_import_does_not_load_the_library(tmp_path: Path) -> None:
    # Naming a missing file makes any attempt to load the library fail.
    env = os.environ | {_native.LIBRARY_ENV_VAR: str(tmp_path / "missing")}

    subprocess.run([sys.executable, "-c", "import mitto"], env=env, check=True)
