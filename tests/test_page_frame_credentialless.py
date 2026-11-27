"""Tests of credentialless iframes in a Page, on a local server.

Each case of tests/data/frames_credentialless_cases.json runs in a Page of its own Session at an HTTP origin, with the
response headers it names, and makes frames whose credentialless attribute is set and others whose is not: what their
windows say, what cookies, storage, channels and workers they share, and what an embedder policy lets them load. Each
case holds what headless Chrome for Testing 157.0.8103.79 gave for the same script, each in a profile of its own.
"""

import http.server
import json
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_DATA = json.loads((Path(__file__).parent / "data" / "frames_credentialless_cases.json").read_text())
_CASES: dict[str, Any] = _DATA["cases"]
# What a case needs that the Runtime does not have yet: a frame of another origin, an opaque one among them, posts its
# page no message (#286), and a frame shows no blob: URL's document (#283).
_KNOWN_GAPS = {
    "blob": "https://github.com/darionxt/mitto/issues/283",
    "cross": "https://github.com/darionxt/mitto/issues/286",
    "data": "https://github.com/darionxt/mitto/issues/286",
    "iso": "https://github.com/darionxt/mitto/issues/286",
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


class _Server(QuietServer):
    # The path, Referer and Cookie of each request of /echo, as it came.
    seen: list[tuple[str, str | None, str | None]]


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        if self.path.startswith("/echo"):
            cast(_Server, self.server).seen.append((self.path, self.headers.get("Referer"), self.headers.get("Cookie")))
            # The Cookie line of the request, to any origin that may read it, and the cookie that its query sets.
            headers = [
                ("Access-Control-Allow-Origin", self.headers.get("Origin", "*")),
                ("Access-Control-Allow-Credentials", "true"),
                ("Cross-Origin-Resource-Policy", "cross-origin"),
            ]
            if "set=" in self.path:
                headers.append(("Set-Cookie", f"{self.path.partition('set=')[2]}; Path=/"))
            self._send("application/json", json.dumps(self.headers.get("Cookie")), headers)
            return
        if self.path.startswith("/case/"):
            case = _CASES[self.path.removeprefix("/case/")]
            self._send("text/html", "<!doctype html>", [(name, value) for name, value in case["headers"]])
            return
        routes = dict(_DATA["routes"])
        for case in _CASES.values():
            routes.update(case.get("routes", {}))
        route = routes.get(self.path)
        if route is None:
            self._send("text/plain", "", [], status=404)
            return
        self._send(route["type"], route["body"], [(name, value) for name, value in route["headers"]])

    def _send(self, kind: str, body: str, headers: list[tuple[str, str]], status: int = 200) -> None:
        payload = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[_Server]:
    server = _Server(("127.0.0.1", 0), _Handler)
    server.seen = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server
    server.shutdown()
    server.server_close()


def _run(server: _Server, name: str, body: str) -> Any:
    script = f"(async () => {{ {_DATA['prelude']}{body} }})()"
    url = f"http://127.0.0.1:{server.server_port}/case/{name}"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        return page.evaluate(script, timeout=_TIMEOUT_SECONDS)


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize(
    "name",
    [
        pytest.param(name, marks=pytest.mark.xfail(reason=_KNOWN_GAPS[name], strict=True))
        if name in _KNOWN_GAPS
        else name
        for name in _CASES
    ],
)
def test_a_credentialless_iframe_is_chrome_s(server: _Server, name: str) -> None:
    assert _run(server, name, _CASES[name]["body"]) == _CASES[name]["chrome"]


@pytest.mark.usefixtures("page_program")
def test_a_credentialless_frame_of_another_origin_loads_under_an_embedder_policy(server: _Server) -> None:
    # Under a page with COOP and COEP require-corp, a credentialless frame of another origin whose response has no COEP
    # commits, and its script asks with no cookies, as Chrome 157 has it in the iso case, which reads that from the
    # frame's message. A plain frame of that origin is blocked, and runs nothing.
    other = f"http://localhost:{server.server_port}"
    body = (
        f"document.cookie = 'top=1; path=/'; await cl('{other}/rep'); await plain('{other}/rep2');"
        " await new Promise((r) => setTimeout(r, 500)); return true;"
    )

    assert _run(server, "iso", body) is True
    assert [(path, cookie) for path, referer, cookie in server.seen if referer == f"{other}/rep"] == [
        ("/echo", None),
        ("/echo", None),
    ]
    assert not [path for path, referer, _ in server.seen if referer == f"{other}/rep2"]


@pytest.mark.usefixtures("page_program")
def test_the_session_neither_lists_nor_deletes_a_credentialless_frames_cookies(server: _Server) -> None:
    # A credentialless frame's cookies are of its nonce's partition, which the Session's cookies leave out, as the
    # Session can write no such partition key, and which a deletion of the Session's cookies leaves alone.
    body = "document.cookie = 'n=top; path=/'; const f = await cl('/page'); f.contentDocument.cookie = 'n=cl; path=/'; window.f = f; return true;"
    url = f"http://127.0.0.1:{server.server_port}/case/attrs"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate(f"(async () => {{ {_DATA['prelude']}{body} }})()", timeout=_TIMEOUT_SECONDS)
        listed = [(cookie.name, cookie.value) for cookie in session.cookies]
        deleted = session.cookies.delete("n")
        left = page.evaluate("[document.cookie, f.contentDocument.cookie]", timeout=_TIMEOUT_SECONDS)

    assert (listed, deleted, left) == ([("n", "top")], 1, ["", "n=cl"])
