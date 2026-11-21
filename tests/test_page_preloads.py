"""Tests of the order of a document's requests: what Chrome's preload scanner fetches ahead of the parser, and how many of
them may run before the body, through Session._open_page() and AsyncSession._open_page(), which stay private until M10.

A local server stands for the page's origin and records each request in the order it came. A test holds the answer to a
request where it needs the parser, or a request, to wait. Each test needs a native library with the Page program beside
it.
"""

import threading
import time
from collections.abc import Callable, Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, Route, png

pytestmark = pytest.mark.usefixtures("both_apis")

_JS = "text/javascript"
_CSS = "text/css"
# Long enough for a request that the Page would make to reach the server, on a busy runner too.
_SETTLE_SECONDS = 0.5


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


def _open_page_holding(
    session: mitto.Session,
    server: RecordingServer,
    url: str,
    held: list[str],
    check: Callable[[], Any],
    then: tuple[str, ...] = (),
) -> Any:
    """Open `url` while the server holds its answers for the paths in `held`, and return what `check` saw once the Page
    asked for the first of them and had time to ask for more, before any answer goes. Opening a Page waits for its load
    event, so the answers go from a thread. The Page stays open until it has asked for each path in `then`, which comes
    after the load event."""
    hold = threading.Event()
    for path in held:
        server.routes[path].hold = hold
    seen: list[Any] = []

    def answer_later() -> None:
        server.wait_for_path(held[0])
        time.sleep(_SETTLE_SECONDS)
        seen.append(check())
        hold.set()

    answerer = threading.Thread(target=answer_later)
    answerer.start()
    try:
        with _open_page(session, url):
            for path in then:
                assert server.wait_for_path(path), path
    finally:
        hold.set()
        answerer.join()
    return seen[0]


def test_the_scanner_fetches_what_follows_a_script_that_the_parser_waits_for(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/held.js", b"", _JS)
    server.image("/after.png", png(2, 2))
    server.route("/after.css", b"p {}", _CSS)
    server.route("/next.js", b"", _JS)
    url = server.page(
        "/",
        "<script src=held.js></script><img src=after.png><link rel=stylesheet href=after.css>"
        "<script src=next.js></script>",
    )

    asked = _open_page_holding(session, server, url, ["/held.js"], lambda: set(server.paths()))

    # The parser waits at the first script, which leaves the rest to the scanner.
    assert {"/after.png", "/after.css", "/next.js"} <= asked
    # The elements take what the scanner fetched.
    for path in ("/held.js", "/after.png", "/after.css", "/next.js"):
        assert server.paths().count(path) == 1, path


def test_before_the_body_requests_below_high_start_two_at_a_time(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/held.js", b"", _JS)
    for name in ("one", "two", "three"):
        server.image(f"/{name}.png", png(1, 1))
    url = server.page(
        "/",
        "<link rel=preload as=image href=one.png><link rel=preload as=image href=two.png>"
        "<link rel=preload as=image href=three.png><script src=held.js></script><body>",
    )

    asked = _open_page_holding(session, server, url, ["/held.js", "/one.png", "/two.png"], lambda: set(server.paths()))

    # The scanner finds all four at once. The first two images start, the third waits while two run, and the script,
    # at High, starts whatever runs; the third goes once an image ends, or the body comes.
    assert {"/one.png", "/two.png", "/held.js"} <= asked
    assert "/three.png" not in asked
    assert server.paths().count("/three.png") == 1


def test_a_style_elements_import_is_fetched_once(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/held.js", b"", _JS)
    server.route("/imported.css", b"p {}", _CSS)
    url = server.page("/", "<script src=held.js></script><style>@import 'imported.css';</style>")

    asked = _open_page_holding(session, server, url, ["/held.js"], lambda: set(server.paths()))

    # The scanner reads the import ahead of the parser, and the sheet takes its response.
    assert "/imported.css" in asked
    assert server.paths().count("/imported.css") == 1


def test_a_url_against_the_base_element_keeps_the_documents_encoding(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/held.js", b"", _JS)
    server.route("/s.js?q=%E9", b"", _JS)
    markup = '<base href="/"><script src=held.js></script><script src="s.js?q=\u00e9"></script>'
    server.routes["/"] = Route(markup.encode("cp1252"), headers=[("Content-Type", "text/html; charset=windows-1252")])

    asked = _open_page_holding(session, server, server.url("/"), ["/held.js"], lambda: set(server.paths()))

    # The scanner resolves against the predicted base in the document's encoding, as Document::CompleteURLWithOverride()
    # does, so the script takes its preload.
    assert "/s.js?q=%E9" in asked
    assert server.paths().count("/s.js?q=%E9") == 1
    assert "/s.js?q=%C3%A9" not in server.paths()


def test_a_csp_meta_element_stops_the_scanner(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/held.js", b"", _JS)
    server.image("/late.png", png(1, 1))
    url = server.page(
        "/",
        '<meta http-equiv="Content-Security-Policy" content="img-src *">'
        "<script src=held.js></script><img src=late.png>",
    )

    asked = _open_page_holding(session, server, url, ["/held.js"], lambda: set(server.paths()))

    # As AllowPreloadingWithCSPMetaTag, which stable does not ship, would let it otherwise.
    assert "/late.png" not in asked
    assert server.paths().count("/late.png") == 1


def test_a_lazy_image_is_never_the_scanners(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/held.js", b"", _JS)
    server.image("/lazy.png", png(1, 1))
    url = server.page("/", "<script src=held.js></script><img src=lazy.png loading=lazy>")

    # It loads at the document's first rendering opportunity, which may come after the load event.
    asked = _open_page_holding(session, server, url, ["/held.js"], lambda: set(server.paths()), then=("/lazy.png",))

    assert "/lazy.png" not in asked
    assert server.paths().count("/lazy.png") == 1
