"""Tests of where a request's Range line goes on the wire: docs/api.md, "The order on a `fetch` and an `xhr`".

Chrome's HTTP cache takes the Range line off a GET of one valid range that it may cache and puts it back last, after
the lines that //net adds, as it writes a range. A Session with a cache has //net's own, which does the same, and one
without does what it would. Each expected value is what Chrome for Testing 157.0.8103.79 gave for the same script, in a
page of its own, over HTTP/1.1. Tests that take the native_library fixture are skipped when this machine has no native
library.
"""

import http.server
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import RequestKind, _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60

# Each request's name, and what its init gives fetch(): a Range line that the cache moves, or one that it leaves where
# it is. A conditional request is the callers' test's, since Chrome adds a Cache-Control line to one, which mitto does
# not yet.
CASES = """[
    ["plain", { headers: [["Range", "bytes=0-1"], ["X-B", "2"]] }],
    ["open", { headers: [["X-B", "2"], ["Range", "bytes=0-"]] }],
    ["suffix", { headers: [["Range", "bytes=-5"]] }],
    ["written", { headers: [["Range", "bytes = 00-01"], ["X-B", "2"]] }],
    ["two ranges", { headers: [["Range", "bytes=0-1,3-4"], ["X-B", "2"]] }],
    ["backwards", { headers: [["Range", "bytes=5-1"], ["X-B", "2"]] }],
    ["another unit", { headers: [["Range", "items=0-1"], ["X-B", "2"]] }],
    ["no-cache", { headers: [["Range", "bytes=0-1"], ["Cache-Control", "no-cache"]] }],
    ["post", { method: "POST", body: "x", headers: [["Range", "bytes=0-1"]] }],
]"""

SCRIPT = f"""
(async () => {{
    const out = [];
    for (const [index, [name, init]] of {CASES}.entries()) {{
        out.push(name + ": " + await (await fetch("/lines?" + index, init)).text());
    }}
    return out.join("\\n");
}})()
"""

AS_CHROME = "\n".join(
    [
        (
            "plain: sec-ch-ua-platform,User-Agent,sec-ch-ua,sec-ch-ua-mobile,X-B,Accept,Sec-Fetch-Site,"
            "Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language,Range | bytes=0-1"
        ),
        (
            "open: sec-ch-ua-platform,User-Agent,sec-ch-ua,sec-ch-ua-mobile,X-B,Accept,Sec-Fetch-Site,"
            "Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language,Range | bytes=0-"
        ),
        (
            "suffix: sec-ch-ua-platform,User-Agent,sec-ch-ua,sec-ch-ua-mobile,Accept,Sec-Fetch-Site,"
            "Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language,Range | bytes=-5"
        ),
        (
            "written: sec-ch-ua-platform,User-Agent,sec-ch-ua,sec-ch-ua-mobile,X-B,Accept,Sec-Fetch-Site,"
            "Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language,Range | bytes=0-1"
        ),
        (
            "two ranges: sec-ch-ua-platform,User-Agent,sec-ch-ua,Range,sec-ch-ua-mobile,X-B,Accept,"
            "Sec-Fetch-Site,Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language | bytes=0-1,3-4"
        ),
        (
            "backwards: sec-ch-ua-platform,User-Agent,sec-ch-ua,Range,sec-ch-ua-mobile,X-B,Accept,Sec-Fetch-Site,"
            "Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language | bytes=5-1"
        ),
        (
            "another unit: sec-ch-ua-platform,User-Agent,sec-ch-ua,Range,sec-ch-ua-mobile,X-B,Accept,"
            "Sec-Fetch-Site,Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language | items=0-1"
        ),
        (
            "no-cache: sec-ch-ua-platform,Cache-Control,User-Agent,sec-ch-ua,sec-ch-ua-mobile,Accept,"
            "Sec-Fetch-Site,Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,Accept-Language,"
            "Range | bytes=0-1"
        ),
        (
            "post: Content-Length,sec-ch-ua-platform,User-Agent,sec-ch-ua,Range,Content-Type,sec-ch-ua-mobile,"
            "Accept,Origin,Sec-Fetch-Site,Sec-Fetch-Mode,Sec-Fetch-Dest,Referer,Accept-Encoding,"
            "Accept-Language | bytes=0-1"
        ),
    ]
)
"""The names of each request's lines, in their order, and its Range line, as Chrome 157 sent them."""


@pytest.fixture
def page_url(native_library: Any) -> Iterator[str]:
    """The URL of an empty page on 127.0.0.1, whose /lines gives the names of the lines that came, in their order, and
    the Range line, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            body = b"<!doctype html><title>t</title><body>"
            if self.path.startswith("/lines"):
                names = ",".join(name for name in self.headers if name.lower() not in {"host", "connection"})
                body = f"{names} | {self.headers.get('Range', 'none')}".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self) -> None:
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            self.do_GET()

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


@pytest.mark.parametrize("cache", [None, "memory"])
def test_a_pages_range_line_goes_where_chrome_sends_it(page_url: str, cache: str | None) -> None:
    with (
        mitto.Session(cache=cache) as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(SCRIPT, timeout=_TIMEOUT_SECONDS) == AS_CHROME


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.NAVIGATION, RequestKind.IMAGE])
def test_a_callers_range_line_goes_where_chromes_cache_puts_it(page_url: str, kind: RequestKind) -> None:
    # //net's cache moves it as Chrome's does, and a Session without a cache moves it the same way.
    requests: list[tuple[str, dict[str, str]]] = [
        ("GET", {"Range": "bytes=0-1", "X-B": "2"}),
        ("GET", {"X-B": "2", "Range": "bytes = 00-"}),
        ("GET", {"Range": "bytes=-5"}),
        ("GET", {"Range": "bytes=0-1,3-4", "X-B": "2"}),
        ("GET", {"Range": "items=1-2", "X-B": "2"}),
        ("GET", {"Range": "bytes=0-1", "If-Range": "x"}),
        ("GET", {"Range": "bytes=0-1", "If-Match": "x"}),
        ("GET", {"Range": "bytes=0-1", "If-Unmodified-Since": "x"}),
        ("GET", {"Range": "bytes=0-1", "If-None-Match": "x"}),
        ("GET", {"Range": "bytes=0-1", "If-Modified-Since": "x"}),
        ("GET", {"Range": "bytes=0-1", "Pragma": "no-cache"}),
        ("HEAD", {"Range": "bytes=0-1"}),
    ]
    seen: dict[str | None, list[str]] = {}
    for cache in (None, "memory"):
        with mitto.Session(cache=cache) as session:
            seen[cache] = [
                session.request(method, f"{page_url}lines?{cache}-{index}", kind=kind, headers=headers).text
                for index, (method, headers) in enumerate(requests)
            ]
    assert seen[None] == seen["memory"]
    assert [lines.endswith(",Range | bytes=0-1") for lines in seen[None]] == [
        True,
        False,
        False,
        False,
        False,
        False,
        False,
        False,
        False,
        False,
        True,
        False,
    ]
    assert seen[None][1].endswith(",Range | bytes=0-")
    assert seen[None][2].endswith(",Range | bytes=-5")
