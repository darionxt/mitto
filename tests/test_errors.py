"""Tests for what a request that fails raises.

Tests that take the native_library fixture go through the real native core to a local server, so they need no network
connection, and are skipped when this machine has no native library.
"""

import ctypes
import http.server
import pickle
import socket
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto._errors import ERROR_TYPES_BY_NAME, ERROR_TYPES_BY_PREFIX, request_error
from mitto._native import PrivateSettings
from tests.local_server import LocalHTTPServer
from tools.capture import HTTP2, CaptureServer

pytestmark = pytest.mark.usefixtures("both_apis")

URL = "http://example.test/"

ERR_CERT_AUTHORITY_INVALID = -202
ERR_CONNECTION_REFUSED = -102
ERR_CONTENT_LENGTH_MISMATCH = -354
ERR_EMPTY_RESPONSE = -324
ERR_INVALID_URL = -300
ERR_NAME_NOT_RESOLVED = -105
ERR_NO_BUFFER_SPACE = -176
ERR_SSL_PROTOCOL_ERROR = -107
ERR_TOO_MANY_REDIRECTS = -310

# Past the lowest code in //net's list, and a positive one that has no meaning.
UNKNOWN_NET_ERRORS = (-999999, 1)

# Every code that //net's list could hold.
NET_ERROR_CODES = range(-1, -1000, -1)

# How many times in all a Session tries a connect that the system refuses, as Windows does now and then.
CONNECT_ATTEMPTS = 3

ALL_REQUEST_ERRORS = (
    mitto.RequestError,
    mitto.NameResolutionError,
    mitto.ConnectionFailedError,
    mitto.TLSError,
    mitto.CertificateError,
    mitto.ProxyError,
    mitto.ProtocolError,
    mitto.TooManyRedirectsError,
    mitto.RequestTimeoutError,
)


class FailingHandler(http.server.BaseHTTPRequestHandler):
    """Answers in the ways that make a request fail, and at /ok as one that succeeds."""

    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        match self.path:
            case "/ok":
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")
            case "/loop":
                self.send_response(302)
                self.send_header("Location", "/loop")
                self.send_header("Content-Length", "0")
                self.end_headers()
            case "/truncated":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(b"too short")
            case _:
                self.close_connection = True

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server_url() -> Iterator[str]:
    """Run a local HTTP server and return its URL."""
    with LocalHTTPServer(("127.0.0.1", 0), FailingHandler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}"
        server.shutdown()


@pytest.mark.usefixtures("native_library")
def test_a_name_that_does_not_resolve() -> None:
    url = "http://nowhere.test/"

    with (
        mitto.Session(_settings={"host_resolver_rules": "MAP nowhere.test ~NOTFOUND"}) as session,
        pytest.raises(mitto.NameResolutionError) as caught,
    ):
        session.get(url)

    assert caught.value.url == url
    assert caught.value.net_error == ERR_NAME_NOT_RESOLVED
    assert caught.value.name == "ERR_NAME_NOT_RESOLVED"


@pytest.mark.usefixtures("native_library")
def test_a_connection_that_is_refused() -> None:
    with socket.create_server(("127.0.0.1", 0)) as listener:
        url = f"http://127.0.0.1:{listener.getsockname()[1]}/"

    with mitto.Session() as session, pytest.raises(mitto.ConnectionFailedError) as caught:
        session.get(url)

    assert caught.value.url == url
    assert caught.value.net_error == ERR_CONNECTION_REFUSED
    assert caught.value.name == "ERR_CONNECTION_REFUSED"


@pytest.mark.usefixtures("native_library")
def test_a_connect_that_the_system_refuses_is_tried_again_with_a_new_socket(server_url: str) -> None:
    with mitto.Session(_settings=PrivateSettings(refused_connects=CONNECT_ATTEMPTS - 1)) as session:
        response = session.get(f"{server_url}/ok")

    assert response.status == 200
    assert response.text == "ok"


@pytest.mark.usefixtures("native_library")
def test_a_connect_that_the_system_refuses_each_time_fails_with_what_it_said(server_url: str) -> None:
    url = f"{server_url}/ok"

    with (
        mitto.Session(_settings=PrivateSettings(refused_connects=CONNECT_ATTEMPTS)) as session,
        pytest.raises(mitto.RequestError) as caught,
    ):
        session.get(url)

    assert type(caught.value) is mitto.RequestError
    assert caught.value.url == url
    assert caught.value.net_error == ERR_NO_BUFFER_SPACE
    assert caught.value.name == "ERR_NO_BUFFER_SPACE"


@pytest.mark.usefixtures("native_library")
def test_an_untrusted_certificate_is_a_tls_error_too() -> None:
    with CaptureServer() as server, mitto.Session() as session, pytest.raises(mitto.CertificateError) as caught:
        session.get(server.url(HTTP2))

    assert isinstance(caught.value, mitto.TLSError)
    assert caught.value.net_error == ERR_CERT_AUTHORITY_INVALID
    assert caught.value.name == "ERR_CERT_AUTHORITY_INVALID"


@pytest.mark.usefixtures("native_library")
def test_too_many_redirects(server_url: str) -> None:
    url = f"{server_url}/loop"

    with mitto.Session() as session, pytest.raises(mitto.TooManyRedirectsError) as caught:
        session.get(url)

    assert caught.value.url == url
    assert caught.value.net_error == ERR_TOO_MANY_REDIRECTS
    assert caught.value.name == "ERR_TOO_MANY_REDIRECTS"


@pytest.mark.usefixtures("native_library")
def test_a_server_that_does_not_speak_tls_is_not_a_certificate_error(server_url: str) -> None:
    with mitto.Session() as session, pytest.raises(mitto.TLSError) as caught:
        session.get(server_url.replace("http://", "https://"))

    assert not isinstance(caught.value, mitto.CertificateError)
    assert caught.value.net_error == ERR_SSL_PROTOCOL_ERROR


@pytest.mark.usefixtures("native_library")
def test_a_response_that_ends_early_is_a_protocol_error(server_url: str) -> None:
    with mitto.Session() as session, pytest.raises(mitto.ProtocolError) as caught:
        session.get(f"{server_url}/truncated")

    assert caught.value.net_error == ERR_CONTENT_LENGTH_MISMATCH


@pytest.mark.usefixtures("native_library")
def test_a_connection_closed_with_no_answer(server_url: str) -> None:
    with mitto.Session() as session, pytest.raises(mitto.ConnectionFailedError) as caught:
        session.get(f"{server_url}/close")

    assert caught.value.net_error == ERR_EMPTY_RESPONSE


@pytest.mark.usefixtures("native_library")
def test_what_fits_no_kind_is_a_plain_request_error() -> None:
    with mitto.Session() as session, pytest.raises(mitto.RequestError) as caught:
        session.get("not a url")

    assert type(caught.value) is mitto.RequestError
    assert caught.value.net_error == ERR_INVALID_URL
    assert caught.value.name == "ERR_INVALID_URL"


@pytest.mark.usefixtures("native_library")
def test_a_session_works_after_a_request_failed(server_url: str) -> None:
    with mitto.Session() as session:
        with pytest.raises(mitto.ConnectionFailedError):
            session.get(f"{server_url}/close")
        with pytest.raises(mitto.TooManyRedirectsError):
            session.get(f"{server_url}/loop")
        with pytest.raises(mitto.ProtocolError):
            session.get(f"{server_url}/truncated")


def test_the_name_comes_from_the_native_core(native_library: ctypes.CDLL) -> None:
    assert native_library.mitto_net_error_name(ERR_NAME_NOT_RESOLVED) == b"ERR_NAME_NOT_RESOLVED"
    assert native_library.mitto_net_error_name(0) == b"OK"


@pytest.mark.parametrize("net_error", UNKNOWN_NET_ERRORS)
def test_a_value_with_no_name_has_a_name_that_says_so(native_library: ctypes.CDLL, net_error: int) -> None:
    assert native_library.mitto_net_error_name(net_error) == b"ERR_<unknown>"


def engine_release_names(library: ctypes.CDLL) -> set[str]:
    """Return the name of every net error of the Engine Release."""
    names = {library.mitto_net_error_name(code).decode() for code in NET_ERROR_CODES}
    names.discard("ERR_<unknown>")
    return names


def test_each_name_that_decides_a_type_is_still_in_the_engine_release(native_library: ctypes.CDLL) -> None:
    assert set(ERROR_TYPES_BY_NAME) <= engine_release_names(native_library)


def test_each_family_that_decides_a_type_is_still_in_the_engine_release(native_library: ctypes.CDLL) -> None:
    names = engine_release_names(native_library)

    for prefix in ERROR_TYPES_BY_PREFIX:
        assert any(name.startswith(prefix) for name in names), prefix


@pytest.mark.parametrize(
    ("name", "error_type"),
    [
        ("ERR_NAME_NOT_RESOLVED", mitto.NameResolutionError),
        ("ERR_NAME_RESOLUTION_FAILED", mitto.NameResolutionError),
        ("ERR_DNS_SERVER_FAILURE", mitto.NameResolutionError),
        ("ERR_CONNECTION_REFUSED", mitto.ConnectionFailedError),
        ("ERR_CONNECTION_RESET", mitto.ConnectionFailedError),
        ("ERR_CONNECTION_CLOSED", mitto.ConnectionFailedError),
        ("ERR_ADDRESS_UNREACHABLE", mitto.ConnectionFailedError),
        ("ERR_INTERNET_DISCONNECTED", mitto.ConnectionFailedError),
        ("ERR_NETWORK_CHANGED", mitto.ConnectionFailedError),
        ("ERR_EMPTY_RESPONSE", mitto.ConnectionFailedError),
        ("ERR_SSL_PROTOCOL_ERROR", mitto.TLSError),
        ("ERR_SSL_VERSION_OR_CIPHER_MISMATCH", mitto.TLSError),
        ("ERR_QUIC_HANDSHAKE_FAILED", mitto.TLSError),
        ("ERR_ECH_NOT_NEGOTIATED", mitto.TLSError),
        ("ERR_CERT_AUTHORITY_INVALID", mitto.CertificateError),
        ("ERR_CERT_DATE_INVALID", mitto.CertificateError),
        ("ERR_CERTIFICATE_TRANSPARENCY_REQUIRED", mitto.CertificateError),
        ("ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN", mitto.CertificateError),
        ("ERR_QUIC_CERT_ROOT_NOT_KNOWN", mitto.CertificateError),
        ("ERR_PROXY_CONNECTION_FAILED", mitto.ProxyError),
        ("ERR_PROXY_CERTIFICATE_INVALID", mitto.ProxyError),
        ("ERR_SOCKS_CONNECTION_FAILED", mitto.ProxyError),
        ("ERR_TUNNEL_CONNECTION_FAILED", mitto.ProxyError),
        ("ERR_INVALID_HTTP_RESPONSE", mitto.ProtocolError),
        ("ERR_HTTP2_PROTOCOL_ERROR", mitto.ProtocolError),
        ("ERR_QUIC_PROTOCOL_ERROR", mitto.ProtocolError),
        ("ERR_CONTENT_DECODING_FAILED", mitto.ProtocolError),
        ("ERR_RESPONSE_HEADERS_TOO_BIG", mitto.ProtocolError),
        ("ERR_TOO_MANY_REDIRECTS", mitto.TooManyRedirectsError),
        ("ERR_TIMED_OUT", mitto.RequestTimeoutError),
        ("ERR_CONNECTION_TIMED_OUT", mitto.RequestTimeoutError),
        ("ERR_DNS_TIMED_OUT", mitto.RequestTimeoutError),
        ("ERR_ABORTED", mitto.RequestError),
        ("ERR_INVALID_URL", mitto.RequestError),
        ("ERR_UNSAFE_PORT", mitto.RequestError),
        ("ERR_<unknown>", mitto.RequestError),
    ],
)
def test_a_net_errors_name_decides_the_type(name: str, error_type: type[mitto.RequestError]) -> None:
    assert type(request_error(URL, -1, "GET", name)) is error_type


@pytest.mark.parametrize("error_type", ALL_REQUEST_ERRORS)
def test_each_error_keeps_what_it_is_given(error_type: type[mitto.RequestError]) -> None:
    error = error_type(URL, ERR_INVALID_URL, "POST", "ERR_INVALID_URL")

    assert isinstance(error, mitto.MittoError)
    assert error.url == URL
    assert error.net_error == ERR_INVALID_URL
    assert error.method == "POST"
    assert error.name == "ERR_INVALID_URL"
    assert str(error) == f"POST {URL} failed with ERR_INVALID_URL ({ERR_INVALID_URL})"


@pytest.mark.parametrize("error_type", ALL_REQUEST_ERRORS)
def test_each_error_survives_pickling(error_type: type[mitto.RequestError]) -> None:
    restored = pickle.loads(pickle.dumps(error_type(URL, ERR_INVALID_URL, "POST", "ERR_INVALID_URL")))

    assert type(restored) is error_type
    assert restored.url == URL
    assert restored.net_error == ERR_INVALID_URL
    assert restored.method == "POST"
    assert restored.name == "ERR_INVALID_URL"


def test_the_timeout_error_is_also_the_builtin_one() -> None:
    error = mitto.RequestTimeoutError(URL, -7, "GET", "ERR_TIMED_OUT")

    assert isinstance(error, TimeoutError)
    assert isinstance(error, mitto.RequestError)
    # What an OSError takes from its arguments does not get mixed up with what this one was given.
    assert error.errno is None
    assert error.strerror is None
    assert error.filename is None


@pytest.mark.parametrize("error_type", [t for t in ALL_REQUEST_ERRORS if t is not mitto.RequestTimeoutError])
def test_no_other_error_is_a_builtin_timeout(error_type: type[mitto.RequestError]) -> None:
    assert not issubclass(error_type, TimeoutError)


def test_every_error_of_mitto_is_a_mitto_error() -> None:
    assert issubclass(mitto.NativeLibraryError, mitto.MittoError)
    assert issubclass(mitto.HTTPStatusError, mitto.MittoError)
