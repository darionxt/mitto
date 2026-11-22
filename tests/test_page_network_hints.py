"""Tests of the network hints of a page's links, rel preconnect and dns-prefetch, which the Session follows as Chrome's
browser does, through Session._open_page() and AsyncSession._open_page(), which stay private until M10.

A local server stands for the page's origin, and another for the origin that a link hints at, which records each
connection and the requests it carried. Each test needs a native library with the Page program beside it.
"""

import http.server
import socketserver
import threading
import time
from collections.abc import Iterator
from typing import Any, Self, cast

import pytest

import mitto
from mitto import _native
from mitto._native import PrivateSettings
from mitto._page import Page
from tests.local_certificates import serving
from tests.local_dns_server import LocalDnsServer
from tests.local_proxy import LocalProxy
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, png

pytestmark = pytest.mark.usefixtures("both_apis")

# Long enough for a hint that the Page would give to reach the network, on a busy runner too.
_SETTLE_SECONDS = 1.0
_HINTED_HOST = "hint.test"


class _ConnectionServer(socketserver.ThreadingTCPServer):
    """Answers each GET with a small image over HTTP/1.1, and records, for each connection in the order it came, the
    paths of the requests it carried."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self) -> None:
        self.connections: list[list[str]] = []
        self.changed = threading.Condition()
        super().__init__(("127.0.0.1", 0), _ConnectionHandler)

    def __enter__(self) -> Self:
        threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.shutdown()
        self.server_close()

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self.server_address[1]}"

    def wait_for(self, connections: int, requests: int = 0) -> bool:
        """Return whether at least this many connections and requests have come, waiting for them."""
        with self.changed:
            return self.changed.wait_for(
                lambda: len(self.connections) >= connections and sum(map(len, self.connections)) >= requests,
                TIMEOUT_SECONDS,
            )

    def snapshot(self) -> list[list[str]]:
        with self.changed:
            return [list(paths) for paths in self.connections]


class _ConnectionHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self) -> None:
        super().setup()
        server = cast(_ConnectionServer, self.server)
        with server.changed:
            self.index = len(server.connections)
            server.connections.append([])
            server.changed.notify_all()

    def do_GET(self) -> None:
        server = cast(_ConnectionServer, self.server)
        with server.changed:
            server.connections[self.index].append(self.path)
            server.changed.notify_all()
        body = png(1, 1)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    with RecordingServer() as page_server:
        yield page_server


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip unless a Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def session(page_program: None) -> Iterator[mitto.Session]:
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _load_image(page: Page, url: str) -> None:
    page.evaluate(
        f"new Promise(resolve => {{ const image = new Image(); image.onload = image.onerror = () => resolve(null); "
        f"image.src = '{url}'; }})",
        timeout=TIMEOUT_SECONDS,
    )


@pytest.mark.parametrize("inserted_by", ["parser", "script"])
def test_a_preconnect_opens_the_connection_that_a_request_with_credentials_takes(
    session: mitto.Session, server: RecordingServer, inserted_by: str
) -> None:
    with _ConnectionServer() as hinted:
        link = f"<link rel=preconnect href='{hinted.origin}'>"
        if inserted_by == "parser":
            url = server.page("/", link)
        else:
            url = server.page("/", f"<script>document.head.insertAdjacentHTML('beforeend', `{link}`)</script>")

        with _open_page(session, url) as page:
            assert hinted.wait_for(connections=1)
            _load_image(page, f"{hinted.origin}/image.png")

        # The preload scanner and the link each hint at it, and the second finds the connection that the first opened.
        assert hinted.snapshot() == [["/image.png"]]


def test_an_anonymous_preconnect_opens_a_connection_that_a_request_with_credentials_does_not_take(
    session: mitto.Session, server: RecordingServer
) -> None:
    with _ConnectionServer() as hinted:
        url = server.page("/", f"<link rel=preconnect href='{hinted.origin}' crossorigin>")

        with _open_page(session, url) as page:
            assert hinted.wait_for(connections=1)
            _load_image(page, f"{hinted.origin}/image.png")

        assert hinted.snapshot() == [[], ["/image.png"]]


def test_a_dns_prefetch_resolves_the_name_and_connects_to_nothing(page_program: None, server: RecordingServer) -> None:
    with (
        _ConnectionServer() as hinted,
        LocalDnsServer() as dns,
        mitto.Session(_settings=PrivateSettings(dns_nameserver=dns.address)) as session,
    ):
        port = hinted.server_address[1]
        url = server.page("/", f"<link rel=dns-prefetch href='//{_HINTED_HOST}:{port}'>")

        with _open_page(session, url):
            deadline = time.monotonic() + TIMEOUT_SECONDS
            while _HINTED_HOST not in {query.name for query in dns.queries} and time.monotonic() < deadline:
                time.sleep(0.05)
            time.sleep(_SETTLE_SECONDS)

        assert _HINTED_HOST in {query.name for query in dns.queries}
        assert hinted.snapshot() == []


def test_a_dns_prefetch_behind_a_proxy_resolves_nothing(page_program: None, server: RecordingServer) -> None:
    # A loopback name goes around a proxy, so the page has a name of its own, which the proxy takes to the server.
    page_port = int(server.origin.rsplit(":", 1)[1])
    with (
        serving(LocalProxy(socks=True)) as proxy,
        LocalDnsServer() as dns,
        mitto.Session(
            proxy=f"socks5://127.0.0.1:{proxy.port}", _settings=PrivateSettings(dns_nameserver=dns.address)
        ) as session,
    ):
        server.page("/", f"<link rel=dns-prefetch href='//{_HINTED_HOST}'>")

        with _open_page(session, f"http://page.test:{page_port}/"):
            time.sleep(_SETTLE_SECONDS)

        assert dns.queries == []
