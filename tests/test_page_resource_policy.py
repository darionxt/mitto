"""Tests of Cross-Origin-Resource-Policy and credentialless requests in Pages that a Session opened, on local servers.

A page on one server asks another, of another origin on the same host, for what it holds: the Session holds each response
of a request of mode no-cors to its Cross-Origin-Resource-Policy, with the embedder policy of the document or worker that
asked, as Chrome's network service does, and a document whose embedder policy is credentialless sends no credentials on
such a request. What each page sees is held to what Chrome 157 gave for the same pages (docs/runtime.md, "Cross-origin
isolation").
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.page_server import PNG, TIMEOUT_SECONDS, RecordingServer, Route, png

_Header = tuple[str, str]
_COOP: _Header = ("Cross-Origin-Opener-Policy", "same-origin")
_BLANK = "<!doctype html><title>r</title>"
_FAILED = "TypeError: Failed to fetch"
_JS = "text/javascript"


def _corp(value: str) -> list[_Header]:
    return [("Cross-Origin-Resource-Policy", value)]


def _coep(value: str) -> _Header:
    return ("Cross-Origin-Embedder-Policy", value)


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip unless a Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    """The server of the page."""
    with RecordingServer() as page_server:
        yield page_server


@pytest.fixture
def other() -> Iterator[RecordingServer]:
    """A server of another origin on the same host, which holds a text of each policy."""
    with RecordingServer() as other_server:
        for policy in ("same-origin", "same-site", "cross-origin", "SAME-ORIGIN"):
            other_server.route(f"/{policy}", policy.encode(), "text/plain", _corp(policy))
        other_server.route("/none", b"none", "text/plain")
        other_server.route("/script.js", b"window.ran = true;", _JS, _corp("same-origin"))
        other_server.image("/image.png", png(1, 1), headers=_corp("same-origin"))
        other_server.routes["/redirect"] = Route(
            status=302, headers=[("Location", "/none"), ("Content-Type", PNG), *_corp("same-origin")]
        )
        other_server.route(
            "/cors", b"cors", "text/plain", [("Access-Control-Allow-Origin", "*"), *_corp("same-origin")]
        )
        yield other_server


@pytest.fixture
def session(page_program: None) -> Iterator[mitto.Session]:
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _fetch(page: Page, url: str, init: str = "{mode: 'no-cors'}") -> Any:
    return page.evaluate(
        f"fetch('{url}', {init}).then(response => response.type, error => String(error))", timeout=TIMEOUT_SECONDS
    )


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


def test_a_policy_blocks_a_no_cors_response_from_another_origin(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK)
    server.route("/own", b"own", "text/plain", _corp("same-origin"))

    with _open_page(session, url) as page:
        outcomes = {
            path: _fetch(page, other.url(path))
            for path in ("/same-origin", "/same-site", "/cross-origin", "/SAME-ORIGIN", "/none")
        }
        own = _fetch(page, "/own")

    # An IP address is of no site, so same-site holds only within its origin, as in Chrome.
    assert outcomes == {
        "/same-origin": _FAILED,
        "/same-site": _FAILED,
        "/cross-origin": "opaque",
        "/SAME-ORIGIN": "opaque",
        "/none": "opaque",
    }
    assert own == "basic"


def test_a_script_and_an_image_that_a_policy_blocks_fire_error(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK)
    script = f"""Promise.all([['script', '{other.url("/script.js")}'], ['img', '{other.url("/image.png")}']].map(
        ([tag, src]) => new Promise((resolve) => {{
            const element = document.createElement(tag);
            element.onload = () => resolve('load');
            element.onerror = () => resolve('error');
            element.src = src;
            document.body.append(element);
        }})))"""

    with _open_page(session, url) as page:
        outcome = page.evaluate(script, timeout=TIMEOUT_SECONDS)

    assert outcome == ["error", "error"]


def test_a_redirect_is_held_to_its_policy_and_a_cors_request_is_not(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK)

    with _open_page(session, url) as page:
        redirected = _fetch(page, other.url("/redirect"))
        cors = _fetch(page, other.url("/cors"), "{mode: 'cors'}")

    assert [redirected, cors] == [_FAILED, "cors"]
    assert "/none" not in other.paths()


def test_coep_require_corp_blocks_a_no_cors_response_with_no_policy(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK, [_COOP, _coep("require-corp")])

    with _open_page(session, url) as page:
        outcomes = [_fetch(page, other.url(path)) for path in ("/none", "/cross-origin")]

    assert outcomes == [_FAILED, "opaque"]


def test_coep_credentialless_sends_no_credentials_on_a_no_cors_request_to_another_origin(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK, [_COOP, _coep("credentialless"), ("Set-Cookie", "who=mitto")])
    plain = server.page("/plain", _BLANK)

    with _open_page(session, url) as page:
        credentialless = [
            _fetch(page, other.url("/none"), "{mode: 'no-cors', credentials: 'include'}"),
            _fetch(page, other.url("/cors"), "{mode: 'cors', credentials: 'include'}"),
        ]
    with _open_page(session, plain) as page:
        without_coep = _fetch(page, other.url("/none"), "{mode: 'no-cors', credentials: 'include'}")

    # Without credentials, the response needs no policy, as COEP credentialless has it.
    assert credentialless == ["opaque", _FAILED]
    assert without_coep == "opaque"
    assert [_value(lines, "Cookie") for lines in other.headers_of("/none")] == [None, "who=mitto"]
    assert _value(other.headers_of("/cors")[0], "Cookie") == "who=mitto"


def test_a_worker_holds_its_requests_to_its_own_embedder_policy(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    url = server.page("/", _BLANK)
    target = other.url("/none")
    server.route(
        "/worker.js",
        f"fetch('{target}', {{mode: 'no-cors'}}).then((r) => postMessage(r.type), (e) => postMessage(String(e)));".encode(),
        _JS,
        [_coep("require-corp")],
    )

    with _open_page(session, url) as page:
        outcome = page.evaluate(
            "new Promise((resolve) => { new Worker('/worker.js').onmessage = (e) => resolve(e.data); })",
            timeout=TIMEOUT_SECONDS,
        )

    assert outcome == _FAILED
