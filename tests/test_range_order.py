"""Tests of where a request's Range line goes on the wire: docs/api.md, "The order on a `fetch` and an `xhr`".

Chrome's HTTP cache takes the Range line off a GET of one valid range that it may cache and puts it back after the
lines that //net adds, as it writes a range. A Session with a cache has //net's own, which does the same, and one
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
def server_url(native_library: Any) -> Iterator[str]:
    """The URL of an empty page on 127.0.0.1, whose /lines gives, in its body and in its X-Lines line, the names of the
    lines that came, in their order, and the Range line, and whose /cookie sets a cookie."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            self._answer(head=False)

        def do_HEAD(self) -> None:
            self._answer(head=True)

        def do_POST(self) -> None:
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            self._answer(head=False)

        def _answer(self, head: bool) -> None:
            body = b"<!doctype html><title>t</title><body>"
            self.send_response(200)
            if self.path == "/cookie":
                self.send_header("Set-Cookie", "kept=1; Path=/")
            if self.path.startswith("/lines"):
                names = ",".join(name for name in self.headers if name.lower() not in {"host", "connection"})
                body = f"{names} | {self.headers.get('Range', 'none')}".encode()
                self.send_header("X-Lines", body.decode())
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            if not head:
                self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


@pytest.fixture
def page_url(server_url: str) -> str:
    """The same URL, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    return server_url


@pytest.mark.parametrize("cache", [None, "memory"])
def test_a_pages_range_line_goes_where_chrome_sends_it(page_url: str, cache: str | None) -> None:
    with (
        mitto.Session(cache=cache) as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(SCRIPT, timeout=_TIMEOUT_SECONDS) == AS_CHROME


@pytest.mark.parametrize("kind", [RequestKind.FETCH, RequestKind.NAVIGATION, RequestKind.IMAGE])
def test_a_callers_range_line_goes_where_chromes_cache_puts_it(server_url: str, kind: RequestKind) -> None:
    # //net's cache moves it as Chrome's does, and a Session without a cache moves it the same way: after the lines
    # that //net adds, a stored cookie's among them, or not at all.
    requests: list[tuple[str, dict[str, str], str | None]] = [
        ("GET", {"Range": "bytes=0-1", "X-B": "2"}, "bytes=0-1"),
        ("GET", {"X-B": "2", "Range": "bytes = 00-"}, "bytes=0-"),
        ("GET", {"Range": "bytes=-5"}, "bytes=-5"),
        ("GET", {"Range": "bytes=0-1,3-4", "X-B": "2"}, None),
        ("GET", {"Range": "items=1-2", "X-B": "2"}, None),
        ("GET", {"Range": "bytes=0-1", "If-Range": "x"}, None),
        ("GET", {"Range": "bytes=0-1", "If-Match": "x"}, None),
        ("GET", {"Range": "bytes=0-1", "If-Unmodified-Since": "x"}, None),
        ("GET", {"Range": "bytes=0-1", "If-None-Match": "x"}, None),
        ("GET", {"Range": "bytes=0-1", "If-Modified-Since": "x"}, None),
        ("GET", {"Range": "bytes=0-1", "Pragma": "no-cache"}, "bytes=0-1"),
        ("HEAD", {"Range": "bytes=0-1"}, None),
    ]
    seen: dict[str | None, list[str | None]] = {}
    for cache in (None, "memory"):
        with mitto.Session(cache=cache) as session:
            session.request("GET", f"{server_url}cookie")
            seen[cache] = [
                session.request(method, f"{server_url}lines?{cache}-{index}", kind=kind, headers=headers).headers.get(
                    "X-Lines"
                )
                for index, (method, headers, _) in enumerate(requests)
            ]
    assert seen[None] == seen["memory"]
    for lines, (_, headers, moved) in zip(seen[None], requests, strict=True):
        assert lines is not None
        names, value = lines.split(" | ")
        order = names.split(",")
        after_net = order.index("Accept-Language") < order.index("Range") and order.index("Cookie") < order.index(
            "Range"
        )
        assert (after_net, value) == ((True, moved) if moved else (False, headers["Range"]))
