"""A local server for the tests of a Page's subresources: it answers each path from a route and records the header lines
of each request it gets, in order, so a test can check what the Page sent and in which order."""

import http.server
import struct
import threading
import zlib
from dataclasses import dataclass, field
from typing import Any

from tests.local_server import QuietServer

# Longer than any local load takes, even on a busy runner.
TIMEOUT_SECONDS = 30.0

HTML = "text/html; charset=utf-8"
PNG = "image/png"


def png(width: int, height: int) -> bytes:
    """Return a PNG of `width` by `height` grey pixels."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    rows = b"".join(b"\0" + b"\x80" * width for _ in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


@dataclass
class Route:
    """What the server answers for one path. With `hold`, it answers only once that is set."""

    body: bytes = b""
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: threading.Event | None = None


class RecordingServer:
    """A local server that records the header lines of each request it gets, in order, by path.

    A path without a route gets a 404 of text.
    """

    def __init__(self) -> None:
        self.routes: dict[str, Route] = {}
        self.requests: list[tuple[str, list[tuple[str, str]]]] = []
        self.requested = threading.Condition()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with server.requested:
                    server.requests.append((self.path, list(self.headers.items())))
                    server.requested.notify_all()
                route = server.routes.get(self.path, Route(b"not here", 404, [("Content-Type", "text/plain")]))
                if route.hold is not None:
                    route.hold.wait(TIMEOUT_SECONDS)
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

    def page(self, path: str, markup: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = Route(markup.encode(), headers=[("Content-Type", HTML), *(headers or [])])
        return self.url(path)

    def route(self, path: str, body: bytes, content_type: str, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = Route(body, headers=[("Content-Type", content_type), *(headers or [])])
        return self.url(path)

    def image(self, path: str, body: bytes, *, status: int = 200, headers: list[tuple[str, str]] | None = None) -> str:
        self.routes[path] = Route(body, status, [("Content-Type", PNG), *(headers or [])])
        return self.url(path)

    def headers_of(self, path: str) -> list[list[tuple[str, str]]]:
        """Return the header lines of each request for `path`, in the order they came."""
        with self.requested:
            return [headers for requested, headers in self.requests if requested == path]

    def wait_for_path(self, path: str, timeout: float = TIMEOUT_SECONDS) -> bool:
        """Return whether a request for `path` has come, waiting up to `timeout` for one."""
        with self.requested:
            return self.requested.wait_for(lambda: any(requested == path for requested, _ in self.requests), timeout)

    def paths(self) -> list[str]:
        """Return the path of each request, in the order they came."""
        with self.requested:
            return [path for path, _ in self.requests]

    def __enter__(self) -> "RecordingServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()
