"""Tests of the document that a Page makes of a navigation's response of each type that is not HTML: Session._open_page(),
which stays private until M10.

A local server serves each response, and each test goes through both APIs. The trees are what Chrome 157 made of the same
responses in a top-level tab. They need a native library with the Page program beside it, as CI's native job installs it on
every Host.
"""

import collections
import http.server
import threading
import time
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0
# Long enough that the pieces of a body reach the Page apart.
_PIECE_GAP_SECONDS = 0.4

_TEXT_PRE = '<pre style="word-wrap: break-word; white-space: pre-wrap;">'
_TEXT_HEAD = '<head><meta name="color-scheme" content="light dark"></head>'
_JSON_HEAD = '<head><meta name="color-scheme" content="light dark"><meta charset="utf-8"></head>'


class _Server:
    """A local server that answers each path with a Content-Type and a body, which it sends in pieces when it has more
    than one."""

    def __init__(self) -> None:
        self.routes: dict[str, tuple[str | None, list[bytes]]] = {}
        self.hits: collections.Counter[str] = collections.Counter()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                server.hits[self.path] += 1
                content_type, pieces = server.routes.get(self.path, ("text/plain", [b"not here"]))
                self.send_response(200 if self.path in server.routes else 404)
                if content_type is not None:
                    self.send_header("Content-Type", content_type)
                if len(pieces) == 1:
                    self.send_header("Content-Length", str(len(pieces[0])))
                    self.end_headers()
                    self.wfile.write(pieces[0])
                    return
                self.send_header("Connection", "close")
                self.end_headers()
                for index, piece in enumerate(pieces):
                    if index:
                        time.sleep(_PIECE_GAP_SECONDS)
                    self.wfile.write(piece)
                    self.wfile.flush()

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def serve(self, path: str, content_type: str | None, *pieces: bytes) -> str:
        self.routes[path] = (content_type, list(pieces) or [b""])
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def __enter__(self) -> "_Server":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_Server]:
    with _Server() as local_server:
        yield local_server


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
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


_SUMMARY = (
    "[document.constructor.name, document.contentType, document.compatMode, document.characterSet, document.title]"
)


def test_a_text_response_is_a_pre_of_its_text(session: mitto.Session, server: _Server) -> None:
    # A text type, a script's type and a feed, which Chrome's network service gives as text/plain: the text in a pre,
    # its first line feed kept, its line breaks normalized and nothing of it markup, in no-quirks mode, in the locale's
    # encoding where no charset is given.
    text = server.serve("/a.txt", "text/plain", b"\nline1 <b>&amp;\n\tline2\r\nend")
    script = server.serve("/a.js", "text/javascript", b"var a = '<x>';")
    feed = server.serve("/a.rss", "application/rss+xml", b"<rss/>")

    with _open_page(session, text) as page:
        assert page.evaluate(_SUMMARY) == ["HTMLDocument", "text/plain", "CSS1Compat", "windows-1252", ""]
        assert page.evaluate("document.documentElement.outerHTML") == (
            f"<html>{_TEXT_HEAD}<body>{_TEXT_PRE}\nline1 &lt;b&gt;&amp;amp;\n\tline2\nend</pre></body></html>"
        )
    with _open_page(session, script) as page:
        assert page.evaluate("[document.contentType, document.body.innerHTML]") == [
            "text/javascript",
            f"{_TEXT_PRE}var a = '&lt;x&gt;';</pre>",
        ]
    with _open_page(session, feed) as page:
        assert page.evaluate("[document.contentType, document.body.innerHTML]") == [
            "text/plain",
            f"{_TEXT_PRE}&lt;rss/&gt;</pre>",
        ]


def test_a_json_response_is_its_text_in_a_pre_beside_the_formatters_container(
    session: mitto.Session, server: _Server
) -> None:
    # Each piece of the body that arrives is a text node of its own, with its line breaks as they came, in UTF-8.
    json = server.serve("/a.json", "application/json", b'{"a": [1, 2], "b": "<x>"}')
    pieces = server.serve("/b.json", "application/ld+json", b'{"a":\r\n', b"1}")

    with _open_page(session, json) as page:
        assert page.evaluate(_SUMMARY) == ["HTMLDocument", "application/json", "CSS1Compat", "UTF-8", ""]
        assert page.evaluate("document.documentElement.outerHTML") == (
            f'<html>{_JSON_HEAD}<body><pre>{{"a": [1, 2], "b": "&lt;x&gt;"}}</pre>'
            '<div class="json-formatter-container"></div></body></html>'
        )
        assert page.evaluate("document.querySelector('div').shadowRoot") is None
    with _open_page(session, pieces) as page:
        assert page.evaluate("[...document.querySelector('pre').childNodes].map((node) => node.data)") == [
            '{"a":\r\n',
            "1}",
        ]


def test_an_empty_text_or_json_response_is_an_empty_html_document(session: mitto.Session, server: _Server) -> None:
    for path, content_type in [("/e.txt", "text/plain"), ("/e.json", "application/json")]:
        with _open_page(session, server.serve(path, content_type)) as page:
            assert page.evaluate("document.documentElement.outerHTML") == "<html><head></head><body></body></html>"


def test_a_media_response_is_a_video_element_that_would_play_it(session: mitto.Session, server: _Server) -> None:
    # Audio too goes in a video element, with the response's type, whether or not the body is a playable file.
    for path, content_type in [("/v.mp4", "video/mp4"), ("/a.mp3", "audio/mpeg"), ("/e.webm", "video/webm")]:
        url = server.serve(path, content_type, b"" if path == "/e.webm" else b"\0\0\0\0")
        with _open_page(session, url) as page:
            assert page.evaluate("[document.contentType, document.compatMode, document.readyState]") == [
                content_type,
                "CSS1Compat",
                "complete",
            ]
            assert page.evaluate("document.documentElement.outerHTML") == (
                '<html><head><meta name="viewport" content="width=device-width"></head><body>'
                '<video controls="" autoplay="" name="media" crossorigin="anonymous">'
                f'<source src="{url}" type="{content_type}"></video></body></html>'
            )


def test_an_image_response_is_an_img_of_its_url(session: mitto.Session, server: _Server) -> None:
    # The tree and the inline styles that Chrome gives the image's document, whose img takes the response itself and
    # fetches nothing. A PNG that stops after its signature has no size, so the document keeps its title.
    url = server.serve("/i.png", "image/png", b"\x89PNG\r\n\x1a\n")
    empty = server.serve("/e.png", "image/png")

    with _open_page(session, url) as page:
        assert page.evaluate("[document.contentType, document.compatMode, document.readyState]") == [
            "image/png",
            "CSS1Compat",
            "complete",
        ]
        assert page.evaluate(
            "[document.documentElement.getAttribute('style'), document.head.outerHTML,"
            " document.body.getAttribute('style'), document.body.childNodes.length, document.images[0].src]"
        ) == [
            "height: 100%;",
            '<head><meta name="viewport" content="width=device-width, minimum-scale=0.1"></head>',
            "margin: 0px; height: 100%; background-color: rgb(14, 14, 14);",
            1,
            url,
        ]
        assert page.evaluate("[document.title, document.images[0].complete, document.images[0].naturalWidth]") == [
            "",
            True,
            0,
        ]
    assert server.hits["/i.png"] == 1
    # An empty image has no tree at all.
    with _open_page(session, empty) as page:
        assert page.evaluate("[document.childNodes.length, document.readyState, document.compatMode]") == [
            0,
            "complete",
            "CSS1Compat",
        ]


def test_an_image_documents_title_has_its_files_name_and_size(session: mitto.Session, server: _Server) -> None:
    # A 3x2 PNG, as Chrome titles its document once the image has loaded: the URL's file, decoded, or the last part of a
    # path that ends in a slash, then the size.
    png = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x03\x00\x00\x00\x02\x08\x02\x00\x00\x00\x12\x16\xf1M\x00\x00\x00\x10IDATx\x9cc\xf8\xcf\xc0\x00A\x0cp\x16\x00A\xd2\x05\xfb\x87\xf0\xb9H\x00\x00\x00\x00IEND\xaeB`\x82"
    named = server.serve("/a%C3%A9%20b.png", "image/png", png)
    directory = server.serve("/dir/", "image/png", png)

    for url, title in ((named, "a\u00e9 b.png (3\u00d72)"), (directory, "dir (3\u00d72)")):
        with _open_page(session, url) as page:
            assert page.evaluate(
                "[document.title, document.head.outerHTML, document.images[0].complete,"
                " document.images[0].naturalWidth, document.images[0].naturalHeight, document.images[0].src]"
            ) == [
                title,
                '<head><meta name="viewport" content="width=device-width, minimum-scale=0.1">'
                f"<title>{title}</title></head>",
                True,
                3,
                2,
                url,
            ]
    assert server.hits["/a%C3%A9%20b.png"] == 1
    assert server.hits["/dir/"] == 1


def test_a_response_of_another_type_that_chrome_shows_is_read_as_html(session: mitto.Session, server: _Server) -> None:
    # XSL is no XML type to Blink, so its markup is HTML, and the document keeps the response's type.
    with _open_page(session, server.serve("/a.xsl", "text/xsl", b"<b>x</b>")) as page:
        assert page.evaluate("[document.contentType, document.documentElement.outerHTML]") == [
            "text/xsl",
            "<html><head></head><body><b>x</b></body></html>",
        ]
