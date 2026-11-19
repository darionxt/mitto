"""Tests of what a Page's link elements preload, what reuses a resource of the same URL, and the document's referrer
policy, through Session._open_page() and AsyncSession._open_page(), which stay private until M10.

A local server stands for the page's origin and records each request, and a second one, on the same host, for another
origin. Where a caller can ask for the same request, the lines that the Page sent are checked against the caller's, as
the Session's Request Kinds make both. Each test needs a native library with the Page program beside it.
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, png

pytestmark = pytest.mark.usefixtures("both_apis")

_CSS = "text/css"
_JS = "text/javascript"
_WOFF2 = "font/woff2"


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    with RecordingServer() as page_server:
        yield page_server


@pytest.fixture
def other() -> Iterator[RecordingServer]:
    with RecordingServer() as other_server:
        yield other_server


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session through each API in turn, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


def test_each_preload_goes_as_a_callers_request_of_its_kind(session: mitto.Session, server: RecordingServer) -> None:
    style = server.route("/style.css", b"p {}", _CSS)
    script = server.route("/script.js", b"window.preloaded = 1;", _JS)
    image = server.image("/image.png", png(1, 1))
    font = server.route("/font.woff2", b"wOF2", _WOFF2)
    url = server.page(
        "/",
        f'<link rel="preload" href="{style}" as="style">'
        f'<link rel="preload" href="{script}" as="script">'
        f'<link rel="preload" href="{image}" as="image">'
        f'<link rel="preload" href="{font}" as="font" type="font/woff2" crossorigin>'
        '<link rel="preload" href="/nothing" as="unknown">',
    )

    with _open_page(session, url) as page:
        # A preload holds the window's load event back no more than Chrome's does, so the Page stays open until each
        # has come.
        for path in ("/style.css", "/script.js", "/image.png", "/font.woff2"):
            assert server.wait_for_path(path), path
        warnings = [message.text for message in page.console_messages if message.level == "warning"]

    for path, kind in (("/style.css", "style"), ("/script.js", "script"), ("/image.png", "image")):
        session.get(server.url(path), kind=kind, initiator=url)
        page_lines, caller_lines = server.headers_of(path)
        assert page_lines == caller_lines, path
    # A preloaded font is High, where @font-face's is VeryHigh, which HTTP/1.1 does not show.
    (font_lines,) = server.headers_of("/font.woff2")
    assert _value(font_lines, "Sec-Fetch-Mode") == "cors"
    assert _value(font_lines, "Sec-Fetch-Dest") == "font"
    assert _value(font_lines, "Origin") == server.origin
    assert server.headers_of("/nothing") == []
    assert warnings == ["<link rel=preload> must have a valid `as` value"]


def test_a_prefetch_goes_as_a_callers_request_of_the_kind_prefetch_and_fires_load_at_its_link(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/next.html", b"<p>next</p>", "text/html")
    url = server.page("/", "<p>prefetch</p>")

    with _open_page(session, url) as page:
        event = page.evaluate(
            """new Promise(resolve => {
              const link = Object.assign(document.createElement('link'), {rel: 'prefetch', href: 'next.html'});
              link.onload = () => resolve('load');
              link.onerror = () => resolve('error');
              document.head.append(link);
            })""",
            timeout=TIMEOUT_SECONDS,
        )

    assert event == "load"
    session.get(server.url("/next.html"), kind="prefetch", initiator=url)
    page_lines, caller_lines = server.headers_of("/next.html")
    assert page_lines == caller_lines
    assert _value(page_lines, "Sec-Purpose") == "prefetch"


def test_a_preload_fires_load_or_error_at_its_link(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/found.css", b"p {}", _CSS)
    url = server.page("/", "<p>events</p>")

    with _open_page(session, url) as page:
        events = page.evaluate(
            """Promise.all(['found.css', 'missing.css'].map(href => new Promise(resolve => {
              const link = Object.assign(document.createElement('link'), {rel: 'preload', as: 'style', href});
              link.onload = () => resolve('load');
              link.onerror = () => resolve('error');
              document.head.append(link);
            })))""",
            timeout=TIMEOUT_SECONDS,
        )

    # A style sheet fails on an error status, as Blink's CSSStyleSheetResource does.
    assert events == ["load", "error"]


def test_a_script_takes_its_preload_and_two_scripts_of_one_url_take_one_request(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/preloaded.js", b"window.count = (window.count || 0) + 1;", _JS)
    server.route("/twice.js", b"window.twice = (window.twice || 0) + 1;", _JS)
    url = server.page(
        "/",
        '<link rel="preload" href="preloaded.js" as="script">'
        '<script src="preloaded.js"></script>'
        '<script src="twice.js"></script><script src="twice.js"></script>',
    )

    with _open_page(session, url) as page:
        counts = page.evaluate("[window.count, window.twice]", timeout=TIMEOUT_SECONDS)

    assert counts == [1, 2]
    assert server.paths().count("/preloaded.js") == 1
    assert server.paths().count("/twice.js") == 1


def test_a_meta_element_sets_the_documents_referrer_policy(
    session: mitto.Session, server: RecordingServer, other: RecordingServer
) -> None:
    other.image("/image.png", png(1, 1))
    other.route("/fetched", b"fetched", "text/plain", [("Access-Control-Allow-Origin", "*")])
    url = server.page(
        "/page?query",
        f'<meta name="referrer" content="origin"><img src="{other.url("/image.png")}">'
        '<meta name="referrer" content="nonsense">',
    )

    with _open_page(session, url) as page:
        page.evaluate(f"fetch('{other.url('/fetched')}').then(response => response.text())", timeout=TIMEOUT_SECONDS)
        errors = [message.text for message in page.console_messages if message.level == "error"]

    (image_lines,) = other.headers_of("/image.png")
    assert _value(image_lines, "Referer") == server.origin + "/"
    (fetch_lines,) = other.headers_of("/fetched")
    assert _value(fetch_lines, "Referer") == server.origin + "/"
    assert errors == [
        "Failed to set referrer policy: The value 'nonsense' is not one of 'always', 'default', 'never', "
        "'origin-when-crossorigin', 'no-referrer', 'no-referrer-when-downgrade', 'origin', 'origin-when-cross-origin', "
        "'same-origin', 'strict-origin', 'strict-origin-when-cross-origin', or 'unsafe-url'. The referrer policy has been "
        "left unchanged."
    ]


def test_the_referrer_policy_header_sets_the_documents_policy(session: mitto.Session, server: RecordingServer) -> None:
    server.image("/image.png", png(1, 1))
    url = server.page("/", '<img src="image.png">', headers=[("Referrer-Policy", "unknown-token, no-referrer")])

    with _open_page(session, url):
        pass

    (lines,) = server.headers_of("/image.png")
    assert _value(lines, "Referer") is None
