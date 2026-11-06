"""Tests of a Session's proxy. A loopback name goes around a proxy, so these ask for names that exist nowhere."""

import base64
import socket
from collections.abc import Callable, Generator
from contextlib import AbstractContextManager, contextmanager
from dataclasses import dataclass
from pathlib import Path

import pytest

import mitto
from mitto._native import PrivateSettings
from mitto._persona import ENGINE_RELEASE_PERSONA, WINDOWS_PERSONAS, load_persona
from tests.local_certificates import TlsServer, generate_leaf, generate_root, serving
from tests.local_proxy import DIGEST_CHALLENGE, LocalProxy
from tools.capture import CASE_HOST, CASES, HTTP1, HTTP2, SAME_SITE_HOST, CaptureServer, normal_form
from tools.parity import BASELINES_DIRECTORY, differences, fingerprint, load_baselines

_CASE = next(case for case in CASES if case.name == "proxy_navigation")
_BASELINES = {
    baseline.protocol: baseline
    for baseline in load_baselines(BASELINES_DIRECTORY)
    if baseline.case == _CASE.name and baseline.release == load_persona(ENGINE_RELEASE_PERSONA).release
}
_USER = "someone"
_PASSWORD = "hunter2"
_BASIC = "Basic " + base64.b64encode(f"{_USER}:{_PASSWORD}".encode()).decode()
_PROXY_HOST = "localhost"
_TARGET_HOST = "target.test"
_ORIGIN = f"http://{_TARGET_HOST}"
_AUTH_REQUESTED = "ERR_PROXY_AUTH_REQUESTED"
_AUTH_UNSUPPORTED = "ERR_PROXY_AUTH_UNSUPPORTED"
_SOCKS_FAILED = "ERR_SOCKS_CONNECTION_FAILED"
_SOCKS5_LOGIN = (_USER.encode(), _PASSWORD.encode())
_SOCKS5_GREETING = b"\x05\x02\x00\x02"
_ENVIRONMENT_PROXY_VARIABLES = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy")


def _with_credentials(proxy: LocalProxy) -> str:
    return f"http://{_USER}:{_PASSWORD}@127.0.0.1:{proxy.port}"


def _socks5_with_credentials(proxy: LocalProxy) -> str:
    return f"socks5://{_USER}:{_PASSWORD}@127.0.0.1:{proxy.port}"


def _private_settings(server: CaptureServer) -> PrivateSettings:
    return PrivateSettings(ignore_certificate_errors_spki_list=[server.spki_hash])


def _free_port() -> int:
    """Return a port that nothing listens on."""
    with socket.create_server(("127.0.0.1", 0)) as listener:
        return listener.getsockname()[1]


@dataclass(frozen=True, slots=True)
class _RunningProxy:
    """A proxy that is listening: its URL for a Session, and the roots that a Session needs to trust it."""

    url: str
    anchors: tuple[str, ...] = ()


@contextmanager
def _http_proxy(_: Path) -> Generator[_RunningProxy]:
    with serving(LocalProxy()) as proxy:
        yield _RunningProxy(f"http://127.0.0.1:{proxy.port}")


@contextmanager
def _http_proxy_with_credentials(_: Path) -> Generator[_RunningProxy]:
    with serving(LocalProxy(authorization=_BASIC)) as proxy:
        yield _RunningProxy(f"http://{_USER}:{_PASSWORD}@127.0.0.1:{proxy.port}")


def _tls_proxy(directory: Path, authorization: str | None = None) -> tuple[LocalProxy, str]:
    root = generate_root()
    proxy = LocalProxy(authorization=authorization)
    proxy.use_tls(directory, generate_leaf(root, _PROXY_HOST))
    return proxy, root.pem


@contextmanager
def _https_proxy(directory: Path) -> Generator[_RunningProxy]:
    proxy, root = _tls_proxy(directory)
    with serving(proxy):
        yield _RunningProxy(f"https://{_PROXY_HOST}:{proxy.port}", (root,))


@contextmanager
def _https_proxy_with_credentials(directory: Path) -> Generator[_RunningProxy]:
    proxy, root = _tls_proxy(directory, _BASIC)
    with serving(proxy):
        yield _RunningProxy(f"https://{_USER}:{_PASSWORD}@{_PROXY_HOST}:{proxy.port}", (root,))


@contextmanager
def _socks5_proxy(_: Path) -> Generator[_RunningProxy]:
    with serving(LocalProxy(socks=True)) as proxy:
        yield _RunningProxy(f"socks5://127.0.0.1:{proxy.port}")


@contextmanager
def _socks5_proxy_with_credentials(_: Path) -> Generator[_RunningProxy]:
    with serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)) as proxy:
        yield _RunningProxy(_socks5_with_credentials(proxy))


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
@pytest.mark.parametrize(
    "start_proxy",
    [
        pytest.param(_http_proxy, id="http"),
        pytest.param(_http_proxy_with_credentials, id="http-with-credentials"),
        pytest.param(_https_proxy, id="https"),
        pytest.param(_https_proxy_with_credentials, id="https-with-credentials"),
        pytest.param(_socks5_proxy, id="socks5"),
        pytest.param(_socks5_proxy_with_credentials, id="socks5-with-credentials"),
    ],
)
def test_a_target_sees_what_chrome_sends_through_each_kind_of_proxy(
    start_proxy: Callable[[Path], AbstractContextManager[_RunningProxy]], protocol: str, tmp_path: Path
) -> None:
    with (
        start_proxy(tmp_path) as proxy,
        CaptureServer() as server,
        mitto.Session(
            proxy=proxy.url, additional_trust_anchors=proxy.anchors, _settings=_private_settings(server)
        ) as session,
    ):
        response = session.get(server.url(protocol, host=CASE_HOST, path=_CASE.path))
        recording = server.recording(protocol, host=CASE_HOST)

    assert response.status == 200
    assert response.protocol == protocol
    found = list(
        differences(_BASELINES[protocol].fingerprint, fingerprint(normal_form(recording, [_CASE]), _CASE.name))
    )
    assert found == []


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("persona", WINDOWS_PERSONAS.values())
def test_connect_sends_the_user_agent_of_the_persona(persona: str) -> None:
    with (
        serving(LocalProxy()) as proxy,
        CaptureServer() as server,
        mitto.Session(
            persona=persona, proxy=f"http://127.0.0.1:{proxy.port}", _settings=_private_settings(server)
        ) as session,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert [name for name, _ in proxy.requests[0].headers] == ["Host", "Proxy-Connection", "User-Agent"]
    assert proxy.requests[0].header("Proxy-Connection") == "keep-alive"
    assert proxy.requests[0].header("User-Agent") == load_persona(persona).user_agent


@pytest.mark.usefixtures("native_library")
def test_credentials_wait_for_the_proxy_to_ask_and_then_answer_it_as_chrome_does() -> None:
    with (
        serving(LocalProxy(authorization=_BASIC)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    first, second = proxy.requests
    assert first.header("Proxy-Authorization") is None
    assert second.request_line == first.request_line
    assert [name for name, _ in second.headers] == ["Host", "Proxy-Connection", "User-Agent", "Proxy-Authorization"]
    assert second.header("Proxy-Authorization") == _BASIC


@pytest.mark.usefixtures("native_library")
def test_a_later_tunnel_sends_the_credentials_without_being_asked() -> None:
    with (
        serving(LocalProxy(authorization=_BASIC)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        session.get(server.url(HTTP1, host=CASE_HOST))
        session.get(server.url(HTTP1, host=SAME_SITE_HOST))

    assert len(proxy.requests) == 3
    assert proxy.requests[2].target.startswith(SAME_SITE_HOST)
    assert proxy.requests[2].header("Proxy-Authorization") == _BASIC


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("written", "meant"),
    [
        pytest.param("us%40er:p%3Ass", "us@er:p:ss", id="percent-encoded"),
        pytest.param("user", "user:", id="no-password"),
        pytest.param(":pass", ":pass", id="no-username"),
        pytest.param("%C3%A9:%E2%82%AC", "é:€", id="utf-8"),
    ],
)
def test_the_credentials_of_a_proxy_url_are_what_it_gets(written: str, meant: str) -> None:
    authorization = "Basic " + base64.b64encode(meant.encode()).decode()
    with (
        serving(LocalProxy(authorization=authorization)) as proxy,
        mitto.Session(proxy=f"http://{written}@127.0.0.1:{proxy.port}") as session,
    ):
        response = session.get(f"{_ORIGIN}/")

    assert response.status == 200


@pytest.mark.usefixtures("native_library")
def test_a_digest_challenge_runs_as_in_chrome() -> None:
    with (
        serving(LocalProxy(authorization="", challenge=DIGEST_CHALLENGE)) as proxy,
        mitto.Session(proxy=_with_credentials(proxy)) as session,
    ):
        session.get(f"{_ORIGIN}/")

    answer = proxy.requests[1].header("Proxy-Authorization") or ""
    assert answer.startswith(f'Digest username="{_USER}", realm="mitto", nonce="abc"')
    assert "response=" in answer
    assert _PASSWORD not in answer


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_credentials_the_proxy_refuses_end_the_request_after_one_answer(protocol: str) -> None:
    with (
        serving(LocalProxy(authorization="Basic other")) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(protocol, host=CASE_HOST))

    assert caught.value.name == _AUTH_REQUESTED
    assert len(proxy.requests) == 2


@pytest.mark.usefixtures("native_library")
def test_a_proxy_that_asks_a_session_with_no_credentials_ends_the_request() -> None:
    with (
        serving(LocalProxy(authorization=_BASIC)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=f"http://127.0.0.1:{proxy.port}", _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name == _AUTH_REQUESTED
    assert len(proxy.requests) == 1


@pytest.mark.usefixtures("native_library")
def test_plain_http_goes_to_the_proxy_with_the_whole_url() -> None:
    with serving(LocalProxy()) as proxy, mitto.Session(proxy=f"http://127.0.0.1:{proxy.port}") as session:
        response = session.get(f"{_ORIGIN}/path?query")

    assert response.status == 200
    assert response.content == b"answered by the proxy"
    assert proxy.requests[0].request_line == f"GET {_ORIGIN}/path?query HTTP/1.1"
    assert proxy.requests[0].header("Host") == _TARGET_HOST
    assert proxy.requests[0].header("Proxy-Connection") == "keep-alive"
    assert proxy.requests[0].header("Connection") is None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("authorization", [_BASIC, "Basic other"])
def test_a_challenge_for_plain_http_is_answered_or_ends_the_request_as_for_a_tunnel(authorization: str) -> None:
    with (
        serving(LocalProxy(authorization=authorization)) as proxy,
        mitto.Session(proxy=_with_credentials(proxy)) as session,
    ):
        if authorization == _BASIC:
            assert session.get(f"{_ORIGIN}/").status == 200
        else:
            with pytest.raises(mitto.ProxyError) as caught:
                session.get(f"{_ORIGIN}/")
            assert caught.value.name == _AUTH_REQUESTED

    assert len(proxy.requests) == 2


@pytest.mark.usefixtures("native_library")
def test_each_hop_of_a_redirect_can_answer_a_proxy_of_its_own() -> None:
    next_host = "next.test"
    with (
        serving(LocalProxy(authorization=_BASIC, challenge_each_host=True)) as proxy,
        mitto.Session(proxy=_with_credentials(proxy)) as session,
    ):
        proxy.redirects[f"{_ORIGIN}/"] = f"http://{next_host}/"
        response = session.get(f"{_ORIGIN}/")

    assert response.url == f"http://{next_host}/"
    # Each host is asked once: the first request of the first has nothing to send, and that of the second sends what the
    # proxy took before.
    assert [request.header("Proxy-Authorization") for request in proxy.requests] == [None, _BASIC, _BASIC, _BASIC]
    assert [request.header("Host") for request in proxy.requests] == [_TARGET_HOST, _TARGET_HOST, next_host, next_host]


@pytest.mark.usefixtures("native_library")
def test_a_socks5_proxy_gets_the_name_and_finds_it_itself() -> None:
    with (
        serving(LocalProxy(socks=True)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=f"socks5://127.0.0.1:{proxy.port}", _settings=_private_settings(server)) as session,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))
        target = server.authority(HTTP2, host=CASE_HOST)

    assert proxy.socks_targets == [target]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_a_socks5_proxy_that_asks_gets_the_credentials_and_then_the_name(protocol: str) -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        response = session.get(server.url(protocol, host=CASE_HOST))
        target = server.authority(protocol, host=CASE_HOST)

    assert response.status == 200
    assert response.protocol == protocol
    assert proxy.socks_greetings == [_SOCKS5_GREETING]
    assert proxy.socks_logins == [_SOCKS5_LOGIN]
    assert proxy.socks_targets == [target]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_credentials_a_socks5_proxy_refuses_end_the_request_after_one_answer(protocol: str) -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=(_USER.encode(), b"other"))) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(protocol, host=CASE_HOST))

    assert caught.value.name == _AUTH_REQUESTED
    assert proxy.socks_logins == [_SOCKS5_LOGIN]
    assert proxy.socks_targets == []


@pytest.mark.usefixtures("native_library")
def test_a_socks5_proxy_that_needs_nothing_never_gets_the_credentials() -> None:
    with (
        serving(LocalProxy(socks=True)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        response = session.get(server.url(HTTP2, host=CASE_HOST))

    assert response.status == 200
    assert proxy.socks_greetings == [_SOCKS5_GREETING]
    assert proxy.socks_logins == []


@pytest.mark.usefixtures("native_library")
def test_a_session_with_no_credentials_offers_a_socks5_proxy_what_chrome_does_and_fails_if_it_asks() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=f"socks5://127.0.0.1:{proxy.port}", _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name == _SOCKS_FAILED
    assert proxy.socks_greetings == [b"\x05\x01\x00"]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("written", "meant"),
    [
        pytest.param("us%40er:p%3Ass", (b"us@er", b"p:ss"), id="percent-encoded"),
        pytest.param("user", (b"user", b""), id="no-password"),
        pytest.param("user:", (b"user", b""), id="empty-password"),
        pytest.param(":pass", (b"", b"pass"), id="no-username"),
        pytest.param("%C3%A9:%E2%82%AC", ("é".encode(), "€".encode()), id="utf-8"),
        pytest.param("é:€", ("é".encode(), "€".encode()), id="not-percent-encoded"),
        pytest.param("%FF:a%00b", (b"\xff", b"a\x00b"), id="bytes-that-are-not-text"),
        pytest.param(f"{'u' * 255}:{'%FF' * 255}", (b"u" * 255, b"\xff" * 255), id="longest"),
    ],
)
def test_the_credentials_of_a_socks5_url_are_the_bytes_it_gets(written: str, meant: tuple[bytes, bytes]) -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=meant)) as proxy,
        CaptureServer() as server,
        mitto.Session(
            proxy=f"socks5://{written}@127.0.0.1:{proxy.port}", _settings=_private_settings(server)
        ) as session,
    ):
        response = session.get(server.url(HTTP2, host=CASE_HOST))

    assert response.status == 200
    assert proxy.socks_logins == [meant]


@pytest.mark.usefixtures("native_library")
def test_a_socks5_proxy_may_send_its_answers_a_byte_at_a_time() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN, socks_splits_answers=True)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        response = session.get(server.url(HTTP2, host=CASE_HOST))

    assert response.status == 200
    assert proxy.socks_logins == [_SOCKS5_LOGIN]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("splits_answers", [False, True])
@pytest.mark.parametrize(
    ("method_answer", "status_answer", "name"),
    [
        pytest.param(b"\x05\xff", None, _AUTH_UNSUPPORTED, id="no-acceptable-method"),
        pytest.param(b"\x05\x01", None, _AUTH_UNSUPPORTED, id="another-method"),
        pytest.param(b"\x04\x02", None, _SOCKS_FAILED, id="another-version"),
        pytest.param(b"\x05", None, _SOCKS_FAILED, id="closes-inside-the-answer-to-the-greeting"),
        pytest.param(b"", None, _SOCKS_FAILED, id="closes-after-the-greeting"),
        pytest.param(None, b"\x01\x01", _AUTH_REQUESTED, id="refuses"),
        pytest.param(None, b"\x01\xff", _AUTH_REQUESTED, id="refuses-with-another-status"),
        pytest.param(None, b"\x05\x00", _SOCKS_FAILED, id="status-of-another-version"),
        pytest.param(None, b"\x01", _SOCKS_FAILED, id="closes-inside-the-status"),
        pytest.param(None, b"", _SOCKS_FAILED, id="closes-after-the-credentials"),
    ],
)
def test_what_a_socks5_proxy_answers_decides_the_error(
    method_answer: bytes | None, status_answer: bytes | None, name: str, splits_answers: bool
) -> None:
    proxy = LocalProxy(
        socks=True,
        socks_credentials=_SOCKS5_LOGIN,
        socks_method_answer=method_answer,
        socks_status_answer=status_answer,
        socks_splits_answers=splits_answers,
    )
    with (
        serving(proxy),
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name == name
    assert proxy.socks_logins == ([] if method_answer is not None else [_SOCKS5_LOGIN])
    assert proxy.socks_targets == []


@pytest.mark.usefixtures("native_library")
def test_a_connection_that_breaks_during_the_exchange_with_a_socks5_proxy_keeps_its_own_error() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN, socks_resets=True)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ConnectionFailedError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name == "ERR_CONNECTION_RESET"
    assert proxy.socks_logins == []


@pytest.mark.usefixtures("native_library")
def test_a_socks5_proxy_that_fails_the_request_after_the_credentials_is_an_error_of_the_proxy() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN, socks_refuses_requests=True)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name == _SOCKS_FAILED
    assert proxy.socks_logins == [_SOCKS5_LOGIN]


@pytest.mark.usefixtures("native_library")
def test_each_tunnel_through_a_socks5_proxy_gives_the_credentials_again() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        session.get(server.url(HTTP1, host=CASE_HOST))
        session.get(server.url(HTTP1, host=SAME_SITE_HOST))
        targets = [server.authority(HTTP1, host=host) for host in (CASE_HOST, SAME_SITE_HOST)]

    assert proxy.socks_logins == [_SOCKS5_LOGIN, _SOCKS5_LOGIN]
    assert proxy.socks_targets == targets


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("protocol", [HTTP1, HTTP2])
def test_a_request_around_a_socks5_proxy_goes_on_as_with_no_proxy_and_gives_it_nothing(protocol: str) -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
    ):
        response = session.get(server.url(protocol))

    assert response.status == 200
    assert response.protocol == protocol
    assert proxy.socks_greetings == []


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "start",
    [
        pytest.param("http://", id="http"),
        pytest.param("https://", id="https"),
        pytest.param("socks5://", id="socks5"),
        pytest.param(f"socks5://{_USER}:{_PASSWORD}@", id="socks5-with-credentials"),
    ],
)
def test_a_proxy_that_is_down_is_an_error_and_not_a_reason_to_go_directly(start: str) -> None:
    with CaptureServer() as server:
        settings = _private_settings(server)
        settings["host_resolver_rules"] = server.host_resolver_rules
        with (
            mitto.Session(proxy=f"{start}127.0.0.1:{_free_port()}", _settings=settings) as session,
            pytest.raises(mitto.ProxyError) as caught,
        ):
            session.get(server.url(HTTP2, host=CASE_HOST))

    assert caught.value.name in {"ERR_PROXY_CONNECTION_FAILED", "ERR_SOCKS_CONNECTION_FAILED"}


@pytest.mark.usefixtures("native_library")
def test_a_loopback_name_goes_around_the_proxy_as_in_chrome() -> None:
    with (
        CaptureServer() as server,
        mitto.Session(proxy=f"http://127.0.0.1:{_free_port()}", _settings=_private_settings(server)) as session,
    ):
        response = session.get(server.url(HTTP2))

    assert response.status == 200


@pytest.mark.usefixtures("native_library")
def test_the_environment_says_nothing_about_a_proxy(monkeypatch: pytest.MonkeyPatch) -> None:
    for name in _ENVIRONMENT_PROXY_VARIABLES:
        monkeypatch.setenv(name, f"http://127.0.0.1:{_free_port()}")

    with CaptureServer() as server:
        settings = _private_settings(server)
        settings["host_resolver_rules"] = server.host_resolver_rules
        with mitto.Session(_settings=settings) as session:
            response = session.get(server.url(HTTP2, host=CASE_HOST))

    assert response.status == 200


@pytest.mark.usefixtures("native_library")
def test_a_certificate_the_proxy_cannot_prove_is_an_error_of_the_proxy(tmp_path: Path) -> None:
    proxy, _ = _tls_proxy(tmp_path)
    with (
        serving(proxy),
        mitto.Session(proxy=f"https://{_PROXY_HOST}:{proxy.port}") as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(f"https://{_TARGET_HOST}/")

    assert caught.value.name == "ERR_PROXY_CERTIFICATE_INVALID"


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "start",
    [
        pytest.param("http://", id="http"),
        pytest.param(f"socks5://{_USER}:{_PASSWORD}@", id="socks5-with-credentials"),
    ],
)
def test_the_connect_timer_covers_a_proxy_that_does_not_answer(start: str) -> None:
    with (
        socket.create_server(("127.0.0.1", 0)) as silent,
        mitto.Session(proxy=f"{start}127.0.0.1:{silent.getsockname()[1]}") as session,
        pytest.raises(mitto.RequestTimeoutError) as caught,
    ):
        session.get(f"https://{_TARGET_HOST}/", timeout=mitto.Timeout(connect=0.2))

    assert caught.value.name == "ERR_CONNECTION_TIMED_OUT"


def _proxy_that_takes_credentials(scheme: str) -> LocalProxy:
    if scheme == "socks5":
        return LocalProxy(socks=True, socks_credentials=_SOCKS5_LOGIN)
    return LocalProxy(authorization=_BASIC)


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("scheme", ["http", "socks5"])
def test_a_server_behind_a_tunnel_cannot_ask_for_the_proxys_credentials(scheme: str, tmp_path: Path) -> None:
    root = generate_root()
    target = TlsServer(tmp_path, generate_leaf(root, _TARGET_HOST))
    target.status = 407
    target.response_headers = [("Proxy-Authenticate", 'Basic realm="not the proxy"')]
    with (
        serving(_proxy_that_takes_credentials(scheme)) as proxy,
        serving(target),
        mitto.Session(
            proxy=f"{scheme}://{_USER}:{_PASSWORD}@127.0.0.1:{proxy.port}", additional_trust_anchors=[root.pem]
        ) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(f"https://{_TARGET_HOST}:{target.server_port}/")

    assert caught.value.name == "ERR_UNEXPECTED_PROXY_AUTH"


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("scheme", ["http", "socks5"])
def test_a_servers_own_challenge_is_a_response_that_the_proxys_credentials_do_not_answer(
    scheme: str, tmp_path: Path
) -> None:
    root = generate_root()
    target = TlsServer(tmp_path, generate_leaf(root, _TARGET_HOST))
    target.status = 401
    target.response_headers = [("WWW-Authenticate", 'Basic realm="the server"')]
    with (
        serving(_proxy_that_takes_credentials(scheme)) as proxy,
        serving(target),
        mitto.Session(
            proxy=f"{scheme}://{_USER}:{_PASSWORD}@127.0.0.1:{proxy.port}", additional_trust_anchors=[root.pem]
        ) as session,
    ):
        response = session.get(f"https://{_TARGET_HOST}:{target.server_port}/")

    assert response.status == 401


@pytest.mark.parametrize(
    "proxy",
    [
        pytest.param("ftp://host:21", id="another-scheme"),
        pytest.param("socks4://host:1080", id="socks4"),
        pytest.param("socks5h://host:1080", id="socks5h"),
        pytest.param("socks://host:1080", id="socks"),
        pytest.param("quic://host:443", id="quic"),
        pytest.param("direct://", id="direct"),
        pytest.param("host:8080", id="no-scheme"),
        pytest.param(f"{_USER}:{_PASSWORD}@host:8080", id="credentials-and-no-scheme"),
        pytest.param("", id="empty"),
    ],
)
def test_a_proxy_of_no_kind_that_a_session_has_is_refused_by_name(proxy: str) -> None:
    with pytest.raises(ValueError, match="http, https or socks5") as caught:
        mitto.Session(proxy=proxy)

    assert _PASSWORD not in str(caught.value)
    assert _USER not in str(caught.value)


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "proxy",
    [
        pytest.param(f"http://{_USER}:{_PASSWORD}@host:99999", id="bad-port"),
        pytest.param(f"socks5://{_USER}:{_PASSWORD}@host:99999", id="socks5-bad-port"),
        pytest.param(f"socks5://{_USER}:{_PASSWORD}{'x' * 255}@host:1080", id="socks5-password-does-not-fit"),
        pytest.param(f"socks5://{_USER}{'x' * 255}:{_PASSWORD}@host:1080", id="socks5-username-does-not-fit"),
    ],
)
def test_a_refused_proxy_keeps_its_password_out_of_the_error(proxy: str) -> None:
    with pytest.raises(ValueError, match="refused the settings") as caught:
        mitto.Session(proxy=proxy)

    assert _PASSWORD not in str(caught.value)
    assert _USER not in str(caught.value)
    assert caught.value.__cause__ is None


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize("scheme", ["http", "socks5"])
def test_a_request_that_fails_keeps_the_proxys_password_out_of_everything_it_shows(scheme: str) -> None:
    with (
        mitto.Session(proxy=f"{scheme}://{_USER}:{_PASSWORD}@127.0.0.1:{_free_port()}") as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(f"{_ORIGIN}/")

    assert _PASSWORD not in repr(session)
    assert _PASSWORD not in str(caught.value)
    assert _PASSWORD not in repr(caught.value)
    assert caught.value.args == (f"{_ORIGIN}/", caught.value.net_error, "GET", caught.value.name)


@pytest.mark.usefixtures("native_library")
def test_credentials_a_socks5_proxy_refuses_stay_out_of_everything_a_failure_shows() -> None:
    with (
        serving(LocalProxy(socks=True, socks_credentials=(_USER.encode(), b"other"))) as proxy,
        CaptureServer() as server,
        mitto.Session(proxy=_socks5_with_credentials(proxy), _settings=_private_settings(server)) as session,
        pytest.raises(mitto.ProxyError) as caught,
    ):
        session.get(server.url(HTTP2, host=CASE_HOST))

    for shown in (repr(session), str(caught.value), repr(caught.value)):
        assert _USER not in shown
        assert _PASSWORD not in shown
