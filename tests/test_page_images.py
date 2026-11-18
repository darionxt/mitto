"""Tests of a Page's images: what an img element fetches through the Session, which source srcset, sizes and picture
choose, and what a page reads of it, through Session._open_page() and AsyncSession._open_page(), which stay private until
M10.

A local server stands for the page's origin and records each request, and a second one, on the same host, for another
origin. Where a caller can ask for the same request, the lines that the Page sent are checked against the caller's, as
the Session's Request Kinds make both. Each test needs a native library with the Page program beside it.
"""

import http.server
import struct
import threading
import time
import zlib
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any, cast

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_HTML = "text/html; charset=utf-8"
_PNG = "image/png"


def _png(width: int, height: int) -> bytes:
    """Return a PNG of `width` by `height` grey pixels."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    rows = b"".join(b"\0" + b"\x80" * width for _ in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


@dataclass
class _Route:
    """What the server answers for one path. With `hold`, it answers only once that is set."""

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: threading.Event | None = None


class _Server:
    """A local server that records the header lines of each request it gets, in order, by path.

    A path without a route gets a 404 of text.
    """

    def __init__(self) -> None:
        self.routes: dict[str, _Route] = {}
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.requested = threading.Condition()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with server.requested:
                    server.requests.append((self.path, list(self.headers.items())))
                    server.requested.notify_all()
                route = server.routes.get(self.path, _Route(b"not here", 404, [("Content-Type", "text/plain")]))
                if route.hold is not None:
                    route.hold.wait(_TIMEOUT_SECONDS)
                self.send_response(route.status)
                for name, value in route.headers:
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(route.body)))
                self.end_headers()
                self.wfile.write(route.body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"

    def url(self, path: str) -> str:
        return f"{self.origin}{path}"

    def page(self, path: str, markup: str) -> str:
        self.routes[path] = _Route(markup.encode(), headers=[("Content-Type", _HTML)])
        return self.url(path)

    def image(self, path: str, body: bytes, *, status: int = 200, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = _Route(body, status, [("Content-Type", _PNG), *(headers or [])])
        return self.url(path)

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        """Return the header lines of each request for `path`, in the order they came."""
        with self.requested:
            return [headers for requested, headers in self.requests if requested == path]

    def wait_for_path(self, path: str, timeout: float = _TIMEOUT_SECONDS) -> bool:
        """Return whether a request for `path` has come, waiting up to `timeout` for one."""
        with self.requested:
            return self.requested.wait_for(lambda: any(requested == path for requested, _ in self.requests), timeout)

    def paths(self) -> list[str]:
        """Return the path of each request, in the order they came."""
        with self.requested:
            return [path for path, _ in self.requests]

    def __enter__(self) -> "_Server":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_Server]:
    with _Server() as page_server:
        yield page_server


@pytest.fixture
def other() -> Iterator[_Server]:
    with _Server() as other_server:
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
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


# What a script reads of an image once its load or error has fired, or at once if it has.
_SETTLED = """
(image => new Promise(resolve => {
  const read = event => resolve({event, complete: image.complete, currentSrc: image.currentSrc,
                                  naturalWidth: image.naturalWidth, naturalHeight: image.naturalHeight,
                                  width: image.width, height: image.height});
  if (image.complete) {
    read('none');
  } else {
    image.addEventListener('load', () => read('load'));
    image.addEventListener('error', () => read('error'));
  }
}))
"""


def _settled(page: Page, image: str) -> dict[str, Any]:
    """Return what a script reads of the image that the expression `image` gives, once it has loaded or failed."""
    result = page.evaluate(f"{_SETTLED}({image})", timeout=_TIMEOUT_SECONDS)
    assert isinstance(result, dict)
    return cast(dict[str, Any], result)


def test_an_img_in_the_markup_is_fetched_as_a_callers_image_request_and_reads_its_size(
    session: mitto.Session, server: _Server
) -> None:
    image_url = server.image("/cat.png", _png(3, 2))
    url = server.page("/", '<img id="cat" src="cat.png">')

    with _open_page(session, url) as page:
        settled = _settled(page, "document.getElementById('cat')")

    assert settled == {
        "event": "none",
        "complete": True,
        "currentSrc": image_url,
        "naturalWidth": 3,
        "naturalHeight": 2,
        "width": 3,
        "height": 2,
    }
    # The page's request goes as the caller's image request from the page goes.
    session.get(image_url, kind="image", initiator=url)
    page_lines, caller_lines = server.headers_of("/cat.png")
    assert page_lines == caller_lines
    assert _value(page_lines, "Sec-Fetch-Dest") == "image"
    assert _value(page_lines, "Sec-Fetch-Mode") == "no-cors"


def test_the_window_load_event_waits_for_an_image(session: mitto.Session, server: _Server) -> None:
    hold = threading.Event()
    server.image("/slow.png", _png(1, 1))
    server.routes["/slow.png"].hold = hold
    url = server.page(
        "/",
        "<script>window.order = []; addEventListener('load', () => order.push('window'));</script>"
        "<img src='slow.png' onload=\"order.push('img')\">",
    )

    # Opening the Page waits for its load event, so the image comes later, from here.
    def answer_later() -> None:
        server.wait_for_path("/slow.png")
        time.sleep(0.5)
        hold.set()

    answerer = threading.Thread(target=answer_later)
    answerer.start()
    try:
        with _open_page(session, url) as page:
            order = page.evaluate("order", timeout=_TIMEOUT_SECONDS)
    finally:
        hold.set()
        answerer.join()

    assert order == ["img", "window"]


def test_new_image_loads_without_being_inserted_and_one_url_is_fetched_once(
    session: mitto.Session, server: _Server
) -> None:
    server.image("/one.png", _png(4, 4))
    url = server.page("/", "<img src='one.png'><img src='one.png'>")

    with _open_page(session, url) as page:
        settled = _settled(page, "Object.assign(new Image(), {src: 'one.png'})")
        images = page.evaluate("[...document.images].map(image => image.complete)", timeout=_TIMEOUT_SECONDS)

    assert settled["naturalWidth"] == 4
    assert images == [True, True]
    # The document's resource of the URL serves each img, as Blink's list of available images does.
    assert server.paths().count("/one.png") == 1


def test_an_image_ignores_its_status_and_fails_only_when_no_decoder_reads_it(
    session: mitto.Session, server: _Server
) -> None:
    server.image("/missing.png", _png(2, 5), status=404)
    url = server.page("/", "<p>status</p>")

    with _open_page(session, url) as page:
        found = _settled(page, "Object.assign(new Image(), {src: 'missing.png'})")
        broken = _settled(page, "Object.assign(new Image(), {src: 'none.png'})")
        empty = page.evaluate(
            "new Promise(resolve => { const image = new Image(); image.onerror = () => resolve('error');"
            " image.src = ''; setTimeout(() => resolve('nothing'), 2000); })",
            timeout=_TIMEOUT_SECONDS,
        )

    assert (found["event"], found["naturalWidth"], found["naturalHeight"]) == ("load", 2, 5)
    assert broken["event"] == "error"
    assert broken["complete"] is True
    assert broken["currentSrc"] == server.url("/none.png")
    assert (broken["naturalWidth"], broken["width"]) == (0, 0)
    # A src of nothing is no URL, and fires error.
    assert empty == "error"


def test_srcset_density_divides_the_natural_size(session: mitto.Session, server: _Server) -> None:
    server.image("/wide.png", _png(40, 20))
    url = server.page("/", "<img id='dense' srcset='wide.png 2x'>")

    with _open_page(session, url) as page:
        settled = _settled(page, "document.getElementById('dense')")

    assert (settled["currentSrc"], settled["naturalWidth"], settled["naturalHeight"]) == (
        server.url("/wide.png"),
        20,
        10,
    )
    # width and height without layout are the natural size at a density of 1.
    assert (settled["width"], settled["height"]) == (40, 20)


def test_a_picture_takes_the_first_source_that_matches_and_that_a_decoder_takes(
    session: mitto.Session, server: _Server
) -> None:
    for name in ("never", "unknown", "chosen", "fallback"):
        server.image(f"/{name}.png", _png(1, 1))
    url = server.page(
        "/",
        "<picture id='picture'>"
        "<source srcset='never.png' media='not all'>"
        "<source srcset='unknown.png' type='image/x-unknown'>"
        "<source srcset='chosen.png' media='(min-width: 0px)' type='image/png'>"
        "<img src='fallback.png'>"
        "</picture>",
    )

    with _open_page(session, url) as page:
        settled = _settled(page, "document.querySelector('#picture img')")

    assert settled["currentSrc"] == server.url("/chosen.png")
    assert "/chosen.png" in server.paths()
    for name in ("never", "unknown", "fallback"):
        assert f"/{name}.png" not in server.paths()


def test_an_svg_image_has_the_natural_size_its_root_gives_and_decode_settles(
    session: mitto.Session, server: _Server
) -> None:
    url = server.page("/", "<p>svg</p>")
    svg = "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' {}></svg>"

    with _open_page(session, url) as page:
        sized = _settled(page, f'Object.assign(new Image(), {{src: "{svg.format("width='60' height='30'")}"}})')
        ratio = _settled(page, f'Object.assign(new Image(), {{src: "{svg.format("viewBox='0 0 10 10'")}"}})')
        unsized = _settled(page, f'Object.assign(new Image(), {{src: "{svg.format("")}"}})')
        decoded = page.evaluate(
            f'Object.assign(new Image(), {{src: "{svg.format("")}"}}).decode().then(() => "decoded")',
            timeout=_TIMEOUT_SECONDS,
        )
        refused = page.evaluate(
            'Object.assign(new Image(), {src: "data:image/svg+xml,<svg></svg>"}).decode()'
            ".then(() => 'decoded', error => `${error.name}: ${error.message}`)",
            timeout=_TIMEOUT_SECONDS,
        )

    assert (sized["naturalWidth"], sized["naturalHeight"]) == (60, 30)
    # A ratio alone fits the default object size, 300 by 150.
    assert (ratio["naturalWidth"], ratio["naturalHeight"]) == (150, 150)
    assert (unsized["naturalWidth"], unsized["naturalHeight"]) == (300, 150)
    assert decoded == "decoded"
    # An svg root outside SVG's namespace is no image.
    assert refused == "EncodingError: The source image cannot be decoded."


def test_an_svg_image_in_utf_16_has_the_natural_size_its_root_gives(session: mitto.Session, server: _Server) -> None:
    root = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'></svg>"
    declared = "<?xml version='1.0' encoding='UTF-16BE'?>" + root
    svg = [("Content-Type", "image/svg+xml")]
    server.routes["/le.svg"] = _Route(b"\xff\xfe" + root.encode("utf-16-le"), headers=svg)
    server.routes["/be.svg"] = _Route(declared.encode("utf-16-be"), headers=svg)
    url = server.page("/", "<p>svg</p>")

    with _open_page(session, url) as page:
        marked = _settled(page, "Object.assign(new Image(), {src: 'le.svg'})")
        declared_only = _settled(page, "Object.assign(new Image(), {src: 'be.svg'})")

    # A byte order mark, or the declaration's first characters, give the encoding away, as libxml finds it.
    for settled in (marked, declared_only):
        assert (settled["event"], settled["naturalWidth"], settled["naturalHeight"]) == ("load", 40, 20)


def test_crossorigin_and_referrerpolicy_change_an_images_request(
    session: mitto.Session, server: _Server, other: _Server
) -> None:
    other.image("/open.png", _png(1, 1), headers=[("Access-Control-Allow-Origin", "*")])
    other.image("/closed.png", _png(1, 1))
    server.image("/private.png", _png(1, 1))
    url = server.page("/", "<p>requests</p>")

    with _open_page(session, url) as page:
        opened = _settled(
            page, f"Object.assign(new Image(), {{crossOrigin: 'anonymous', src: '{other.url('/open.png')}'}})"
        )
        closed = _settled(
            page, f"Object.assign(new Image(), {{crossOrigin: 'anonymous', src: '{other.url('/closed.png')}'}})"
        )
        private = _settled(page, "Object.assign(new Image(), {referrerPolicy: 'no-referrer', src: 'private.png'})")

    assert opened["event"] == "load"
    # One that fails the CORS check is broken.
    assert closed["event"] == "error"
    assert private["event"] == "load"
    (open_lines,) = other.headers_of("/open.png")
    assert _value(open_lines, "Sec-Fetch-Mode") == "cors"
    assert _value(open_lines, "Origin") == server.origin
    (private_lines,) = server.headers_of("/private.png")
    assert _value(private_lines, "Referer") is None


def test_a_lazy_image_loads_at_the_next_rendering_opportunity_once_in_the_document(
    session: mitto.Session, server: _Server
) -> None:
    # Without layout, each lazy image in the document comes near the viewport at the next rendering opportunity, as the
    # owner chose in #284, and one out of the document never does, as in Chrome.
    server.image("/lazy.png", _png(2, 2))
    server.image("/away.png", _png(2, 2))
    url = server.page("/", "<p>lazy</p>")

    with _open_page(session, url) as page:
        result = page.evaluate(
            """(async () => {
              const away = Object.assign(new Image(), {loading: 'lazy', src: 'away.png'});
              const image = Object.assign(new Image(), {loading: 'lazy', src: 'lazy.png'});
              document.body.append(image);
              const waiting = [image.complete, image.currentSrc];
              await new Promise(resolve => { image.onload = resolve; });
              await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
              return [waiting, image.complete, image.naturalWidth, away.complete];
            })()""",
            timeout=_TIMEOUT_SECONDS,
        )

    assert result == [[False, ""], True, 2, False]
    assert "/away.png" not in server.paths()


def test_decode_of_an_image_that_its_document_has_settles_after_it_goes_into_a_picture(
    session: mitto.Session, server: _Server
) -> None:
    # The img takes the document's resource again when it goes into the picture, and hears of its end once, as Blink's
    # counted observers do.
    server.image("/cached.png", _png(1, 1))
    url = server.page("/", "<picture id='picture'></picture>")

    with _open_page(session, url) as page:
        outcome = page.evaluate(
            """(async () => {
              await new Promise(resolve => { Object.assign(new Image(), {onload: resolve, src: 'cached.png'}); });
              const image = Object.assign(new Image(), {src: 'cached.png'});
              const decoded = image.decode().then(() => 'resolved', error => error.name);
              document.getElementById('picture').append(image);
              return decoded;
            })()""",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == "resolved"


def test_a_lazy_image_does_not_hold_the_window_load_event_back(session: mitto.Session, server: _Server) -> None:
    hold = threading.Event()
    server.image("/later.png", _png(1, 1))
    server.routes["/later.png"].hold = hold
    url = server.page("/", "<img id='later' loading='lazy' src='later.png'>")

    try:
        # Opening the Page waits for its load event, which the lazy image's fetch does not hold back.
        with _open_page(session, url) as page:
            assert server.wait_for_path("/later.png")
            waiting = page.evaluate("document.getElementById('later').complete", timeout=_TIMEOUT_SECONDS)
            hold.set()
            settled = _settled(page, "document.getElementById('later')")
    finally:
        hold.set()

    assert waiting is False
    assert settled["naturalWidth"] == 1


def test_an_image_that_lets_its_load_go_no_longer_holds_the_window_load_event_back(
    session: mitto.Session, server: _Server
) -> None:
    # The fetch that no img uses any more is cancelled, as Blink's is once its last client goes.
    hold = threading.Event()
    server.image("/never.png", _png(1, 1))
    server.routes["/never.png"].hold = hold
    url = server.page("/", "<img id='gone' src='never.png'><script>document.getElementById('gone').src = '';</script>")

    try:
        with _open_page(session, url) as page:
            complete = page.evaluate("document.getElementById('gone').complete", timeout=_TIMEOUT_SECONDS)
    finally:
        hold.set()

    assert complete is True
