"""Tests of the subresources whose URLs have a user or a password, on a local server.

A document and a worker refuse most of them, and take the user and the password off the URLs of the others, those of
mode cors. tests/data/credentialed_subresource_cases.json keeps what Chrome 157 did, headless on macOS (157.0.8103.79).
"""

import base64
import html
import http.server
import json
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, quote, urlsplit

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "credentialed_subresource_cases.js").read_bytes()
_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="
)
_BODIES = {
    "png": (_PNG, "image/png"),
    "js": (b"export const url = import.meta.url;", "text/javascript"),
    "classic": (b"", "text/javascript"),
    "throws": (b"throw new Error('thrown');", "text/javascript"),
    "css": (b"body { color: red; }", "text/css"),
    "txt": (b"text", "text/plain"),
    "worker": (b"postMessage(location.href);", "text/javascript"),
}

_PAGE = """<!doctype html><title>c</title>
<script>const parsed = {};</script>
<link rel="icon" href="http://u:p@HOST/a?case=icon&amp;type=png">
<link rel="stylesheet" href="http://u:p@HOST/a?case=parsed-stylesheet&amp;type=css"
  onload="parsed.stylesheet = 'load'" onerror="parsed.stylesheet = 'error'">
<link rel="stylesheet" href="http://u:p@HOST/a?case=parsed-stylesheet-anonymous&amp;type=css" crossorigin
  onload="parsed.anonymous = 'load'" onerror="parsed.anonymous = 'error'">
<body>
<script src="http://u:p@HOST/a?case=parsed-script&amp;type=js" onload="parsed.script = 'load'"
  onerror="parsed.script = 'error'"></script>
<script>parsed.after = parsed.script;</script>
<script src="http://u:p@HOST/a?case=parsed-script-anonymous&amp;type=classic" crossorigin
  onload="parsed.script_anonymous = 'load'" onerror="parsed.script_anonymous = 'error'"></script>
<script type="module" src="http://u:p@HOST/a?case=parsed-module&amp;type=js" onload="parsed.module = 'load'"
  onerror="parsed.module = 'error'"></script>
<script defer src="http://u:p@HOST/a?case=parsed-defer&amp;type=classic" onload="parsed.defer = 'load'"
  onerror="parsed.defer = 'error'"></script>
<img src="http://u:p@HOST/a?case=parsed-img&amp;type=png" onload="parsed.img = 'load'" onerror="parsed.img = 'error'">
<script src="http://HOST/cases.js"></script>"""

_ICON_PAGE = """<!doctype html><title>i</title>
<link rel="icon" href="ICON">"""

DOCUMENTS = {"plain": "", "user": "u:p@"}
"""The users and passwords that the documents' URLs have."""


class CredentialsServer(QuietServer):
    """A server of the page at `/?other=` and of one at `/icon` that only has an icon, with how often each case asked for
    its resource."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        self.host = f"127.0.0.1:{self.server_port}"
        self.requests: dict[str, int] = {}


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        assert isinstance(self.server, CredentialsServer)
        url = urlsplit(self.path)
        query = {name: values[0] for name, values in parse_qs(url.query).items()}
        match url.path:
            case "/":
                self._answer(_PAGE.replace("HOST", self.server.host).encode(), "text/html; charset=utf-8")
            case "/icon":
                icon = f"http://u:p@{self.server.host}/a?case=icon&type=png"
                if "redirect" in query:
                    icon = f"http://{self.server.host}/redirect?to={quote(icon, safe='')}"
                self._answer(_ICON_PAGE.replace("ICON", html.escape(icon)).encode(), "text/html; charset=utf-8")
            case "/cases.js" | "/worker":
                self._answer(_CASES, "text/javascript; charset=utf-8")
            case "/module":
                end = "postMessage('started');" if "worker" in query else ""
                self._answer(f"import '{query['to']}'; {end}".encode(), "text/javascript")
            case "/import":
                self._answer(f"@import url({query['to']});".encode(), "text/css")
            case "/redirect":
                self._answer(b"", "text/plain", status=302, location=query["to"])
            case "/relative":
                self._answer(b"", "text/plain", status=302, location=f"/a?{url.query}")
            case "/a":
                case = query["case"]
                self.server.requests[case] = self.server.requests.get(case, 0) + 1
                self._answer(*_BODIES[query["type"]])
            case _:
                self._answer(b"", "text/plain", status=404)

    def do_POST(self) -> None:
        # A beacon's, which is answered as its GET would be.
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        self.do_GET()

    def _answer(self, body: bytes, content_type: str, *, status: int = 200, location: str | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        # A request of another origin with credentials reads it too.
        self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
        self.send_header("Access-Control-Allow-Credentials", "true")
        self.send_header("Timing-Allow-Origin", "*")
        if location is not None:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def servers() -> Iterator[tuple[CredentialsServer, CredentialsServer]]:
    """The document's server, and one of another origin."""
    servers = (CredentialsServer(), CredentialsServer())
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    yield servers
    for server in servers:
        server.shutdown()
        server.server_close()


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("document", DOCUMENTS)
def test_subresources_with_a_user_or_a_password_go_as_chrome_sends_them(
    servers: tuple[CredentialsServer, CredentialsServer], document: str
) -> None:
    expected = json.loads((_TESTS / "data" / "credentialed_subresource_cases.json").read_text(encoding="utf-8"))
    own, other = servers
    url = f"http://{DOCUMENTS[document]}{own.host}/?other={other.host}"

    with (
        mitto.Session() as session,
        session._open_page(url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        results = page.evaluate("cases", timeout=_TIMEOUT_SECONDS)

    results |= {"requests": own.requests, "requests of the other": other.requests}
    assert results == expected[document]


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("path", ["/icon", "/icon?redirect=1"])
def test_an_icon_that_one_document_refuses_loads_for_the_next(
    servers: tuple[CredentialsServer, CredentialsServer], path: str
) -> None:
    own, _ = servers

    with (
        mitto.Session() as session,
        session._open_page(f"http://{own.host}{path}", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        # The entry of the refused request says that it has ended.
        page.evaluate(
            """(async () => {
              while (!performance.getEntriesByType('resource').length) {
                await new Promise((resolve) => setTimeout(resolve, 10));
              }
            })()""",
            timeout=_TIMEOUT_SECONDS,
        )
        refused = dict(own.requests)
        page.evaluate(f"location.href = 'http://u:p@{own.host}{path}'; 0")
        deadline = time.monotonic() + _TIMEOUT_SECONDS
        while "icon" not in own.requests and time.monotonic() < deadline:
            time.sleep(0.05)

    assert refused == {}
    assert own.requests == {"icon": 1}
