"""Tests of the icon that a Page asks for once its document has loaded, as Chrome's favicon driver does, through
Session._open_page() and AsyncSession._open_page(), which stay private until M10.

A local server stands for the page's origin and records each request. Each test needs a native library with the Page
program beside it.
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, png

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than a Page takes to ask for an icon after its load event, on a busy runner too.
_ICON_SECONDS = 5.0


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    with RecordingServer() as page_server:
        yield page_server


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


def test_a_document_with_no_icon_asks_for_its_origins_favicon(session: mitto.Session, server: RecordingServer) -> None:
    server.image("/favicon.ico", png(16, 16))
    url = server.page("/dir/page?query", "<p>no icon</p>")

    with _open_page(session, url):
        assert server.wait_for_path("/favicon.ico", _ICON_SECONDS)

    # It goes as a caller's request of the kind favicon from the page.
    session.get(server.url("/favicon.ico"), kind="favicon", initiator=url)
    page_lines, caller_lines = server.headers_of("/favicon.ico")
    assert page_lines == caller_lines


def test_the_last_icon_link_wins_and_the_next_is_tried_if_no_image_comes(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.image("/first.png", png(16, 16))
    server.route("/last.png", b"not an image", "text/plain")
    url = server.page(
        "/",
        "<head><link rel=icon href=first.png><link rel='shortcut icon' href=last.png>"
        "<link rel=icon href=print.png media=print></head><p>icons</p>",
    )

    with _open_page(session, url):
        assert server.wait_for_path("/first.png", _ICON_SECONDS)

    # Icons seen later take precedence, one for other media aside; what gives no image leads to the one before it.
    assert [path for path in server.paths() if path.endswith(".png") or path == "/favicon.ico"] == [
        "/last.png",
        "/first.png",
    ]


def test_a_data_icon_asks_for_nothing_and_an_icon_is_asked_for_once(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.image("/icon.png", png(16, 16))
    second = server.page("/second", "<head><link rel=icon href=icon.png></head><p>second</p>")
    url = server.page("/", f"<head><link rel=icon href='data:,'></head><a href='{second}' id=next>next</a>")

    with _open_page(session, url) as page:
        # From a timer, so that the navigation does not end the context of the script that starts it before it returns.
        page.evaluate("setTimeout(() => document.getElementById('next').click()); null", timeout=TIMEOUT_SECONDS)
        page.wait_for_navigation(timeout=TIMEOUT_SECONDS)
        assert server.wait_for_path("/icon.png", _ICON_SECONDS)
        page.evaluate("setTimeout(() => location.reload()); null", timeout=TIMEOUT_SECONDS)
        page.wait_for_navigation(timeout=TIMEOUT_SECONDS)
        page.evaluate("new Promise(resolve => setTimeout(resolve, 1000))", timeout=TIMEOUT_SECONDS)

    # The favicon service keeps what it fetched, so the second load of the page asks for it no more.
    assert "/favicon.ico" not in server.paths()
    assert server.paths().count("/icon.png") == 1
