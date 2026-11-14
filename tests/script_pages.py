"""A local server for pages whose scripts log what runs, which Page tests open and compare with what Chrome gave.

Each case is a page and the paths it fetches. A response can wait until the page sends a mark, fetch('/mark/NAME'), so
that an order does not hang on the network's timing, and a page's body can go out piece by piece. What a page POSTs to
/result is kept, as a run of the same page in Chrome sends what it logged there.
"""

import http.server
import threading
import time
from dataclasses import dataclass, field
from typing import Any

from tests.local_server import QuietServer

__all__ = ["Case", "Route", "ScriptServer"]

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0


@dataclass
class Route:
    """What the server answers for one path.

    With `hold`, the answer waits for that mark. With `pieces`, the body goes out piece by piece, each once the marks
    that it names, separated by commas, have come, and the connection closes at the end.
    """

    body: str = ""
    content_type: str = "text/javascript"
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: str | None = None
    pieces: list[tuple[str | None, str]] | None = None


@dataclass
class Case:
    """A page at / and the paths it fetches."""

    routes: dict[str, Route]


class _BacklogServer(QuietServer):
    """A local server that queues as many connections as a page opens at once.

    A case that opens a connection for each of some twenty scripts would find socketserver's backlog of five refusing
    the rest on a busy macOS runner, which a script reports as an error.
    """

    request_queue_size = 128


class ScriptServer:
    """A local server for one case at a time, which records the path of each request it gets, in order.

    A request for /mark/NAME sets the mark NAME, and sending a route's whole response sets served:PATH. The body of each
    POST to /result goes to `results`.
    """

    def __init__(self) -> None:
        self.case = Case({})
        self.requests: list[str] = []
        self.results: list[str] = []
        self._marks: dict[str, threading.Event] = {}
        self._lock = threading.Lock()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self) -> None:
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if self.path == "/result":
                    with server._lock:
                        server.results.append(body.decode())
                server.answer(self, 200, "text/plain", b"ok")

            def do_GET(self) -> None:
                server.serve(self)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = _BacklogServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def mark(self, name: str) -> threading.Event:
        with self._lock:
            return self._marks.setdefault(name, threading.Event())

    def answer(self, handler: http.server.BaseHTTPRequestHandler, status: int, content_type: str, body: bytes) -> None:
        handler.send_response(status)
        handler.send_header("Content-Type", content_type)
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def serve(self, handler: http.server.BaseHTTPRequestHandler) -> None:
        path = handler.path
        with self._lock:
            self.requests.append(path)
        if path.startswith("/mark/"):
            self.mark(path.removeprefix("/mark/")).set()
            self.answer(handler, 200, "text/plain", b"ok")
            return
        route = self.case.routes.get(path)
        if route is None:
            self.answer(handler, 404, "text/plain", b"not here")
            return
        if route.hold:
            self.mark(route.hold).wait(_TIMEOUT_SECONDS)
        served = self.mark(f"served:{path}")
        if route.pieces is not None:
            handler.send_response(route.status)
            handler.send_header("Content-Type", route.content_type)
            handler.send_header("Connection", "close")
            handler.end_headers()
            for hold, piece in route.pieces:
                if hold:
                    for name in hold.split(","):
                        self.mark(name).wait(_TIMEOUT_SECONDS)
                    # As long as the run in Chrome gave it to take what came before.
                    time.sleep(0.2)
                handler.wfile.write(piece.encode())
                handler.wfile.flush()
            handler.close_connection = True
            return
        handler.send_response(route.status)
        if route.content_type:
            handler.send_header("Content-Type", route.content_type)
        for name, value in route.headers:
            handler.send_header(name, value)
        body = route.body.encode()
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
        handler.wfile.flush()
        served.set()

    def __enter__(self) -> "ScriptServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()
