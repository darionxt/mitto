"""The requests of tests/header_order_cases.js, and what a capture server saw of them, for tests/test_header_order.py."""

import json
from pathlib import Path

from tools.capture import CaptureServer, Run
from tools.capture.page import RequestHead, Response
from tools.capture.server import respond_on

__all__ = ["AS_CHROME", "KINDS", "PAGE_PATH", "SETS", "answer", "cases", "names_of"]

PAGE_PATH = "/order/page"
KINDS = ("fetch", "xhr")
_PREFIX = "/order/"
_TESTS = Path(__file__).parent
SETS: dict[str, dict[str, str]] = {
    "none": {},
    "one": {"X-Alpha": "1"},
    "two": {"Authorization": "Bearer x", "X-Requested-With": "XMLHttpRequest"},
    "three": {"X-Alpha": "1", "X-Beta": "2", "X-Gamma": "3"},
    "reversed": {"X-Gamma": "3", "X-Beta": "2", "X-Alpha": "1"},
    "accept": {"Accept": "application/json", "X-CSRF-Token": "z", "Accept-Language": "de"},
    "cache": {"Cache-Control": "no-cache", "Pragma": "no-cache"},
    "hints": {"DPR": "2", "Save-Data": "on", "Width": "100"},
    "combined": {"x-one": "1, 3", "X-Two": "2"},
    "many": {f"x-{letter}": letter for letter in "abcdefgh"},
    "more": {f"{letter}-header": letter for letter in "abcdefghijklmnopqrstuvwxyz"},
}
"""The lines of each set, in the order that a script sets them."""

AS_CHROME: dict[str, dict[str, list[str]]] = json.loads((_TESTS / "data" / "header_order_cases.json").read_text())
"""The names of the lines that Chrome 157 sent for each request, in their order, by the protocol and the request."""


def cases(kinds: tuple[str, ...]) -> str:
    """Return the script of the requests of these ways to ask."""
    script = (_TESTS / "header_order_cases.js").read_text()
    sets = {name: list(lines.items()) for name, lines in SETS.items()}
    return script.replace("= KINDS;", f"= {json.dumps(kinds)};").replace("= SETS;", f"= {json.dumps(sets)};")


def answer(scheme: str, port: int, request: RequestHead) -> Response:
    """Answer a request of the script's, and any other as the case pages do."""
    if request.target == PAGE_PATH:
        return Response(200, "text/html; charset=utf-8", b"<!doctype html><title>t</title>", "no-store")
    if request.target.startswith(_PREFIX):
        return Response(200, "text/plain", b"answered", "no-store")
    return respond_on(scheme, port, request)


def names_of(server: CaptureServer, run: Run) -> dict[str, list[str]]:
    """Return the names of the header lines of each request of the script's that a server has had on a run's route, in
    their order, without those of the connection and the pseudo-headers."""
    recording = server.recording(run.route, host=run.host)
    return {
        request.target.removeprefix(_PREFIX): [
            name
            for name, _ in request.headers
            if not name.startswith(":") and name.lower() not in {"host", "connection", "content-length"}
        ]
        for connection in recording.connections
        for request in connection.requests
        if request.target.startswith(_PREFIX) and request.target != PAGE_PATH
    }
