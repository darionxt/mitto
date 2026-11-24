"""Tests of what a Page's documents share by their storage key, on a local server.

tests/frames_storage_cases.js runs in a Page at an HTTP origin with frames of its own origin, about:blank, srcdoc and data: URLs,
of another origin of its site, of another site, and within each other, and gives who hears of a change to localStorage or
sessionStorage and of a BroadcastChannel's message, who shares a Web Lock, and what a script still holds of a document that has
gone. tests/data/frames_storage_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the same scripts, and
for tabs whose documents run tests/frames_storage_tab.js, which are Pages of one Session here.
"""

import http.server
import json
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_storage_cases.js").read_text()
_CHILD = (_TESTS / "frames_storage_child.js").read_text()
_TAB = (_TESTS / "frames_storage_tab.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_storage_cases.json").read_text())


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def ports() -> Iterator[tuple[int, int]]:
    """The ports of two servers on 127.0.0.1 of an empty page, the child script, and documents at /child that run it."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            kind, body = "text/html", "<!doctype html><title>t</title><body>"
            if self.path == "/child.js":
                kind, body = "text/javascript", _CHILD
            elif self.path.startswith("/child"):
                body = f"<!doctype html><body><script>{_CHILD}</script>"
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, format: str, *args: Any) -> None:
            pass

    servers = [QuietServer(("127.0.0.1", 0), Handler) for _ in range(2)]
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    yield servers[0].server_port, servers[1].server_port
    for server in servers:
        server.shutdown()
        server.server_close()


@pytest.mark.usefixtures("page_program")
def test_what_a_page_and_its_frames_share_is_chrome_s(ports: tuple[int, int]) -> None:
    url = f"http://127.0.0.1:{ports[0]}/app/page?{ports[1]}"
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_what_the_frames_of_a_session_s_pages_share_is_chrome_s(ports: tuple[int, int]) -> None:
    # Two Pages of one origin and one of another site, each with a frame of its own origin, one of the other site, and one of
    # its own origin within one of the other site, whose storage is the Page's site's and no other's.
    here, there = f"http://127.0.0.1:{ports[0]}", f"http://localhost:{ports[0]}"
    origins = {"1": (here, there), "2": (here, there), "3": (there, here)}
    with mitto.Session() as session:
        pages = {
            tab: session._open_page(f"{own}/app/page", timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]
            for tab, (own, _) in origins.items()
        }

        def answer(where: str, message: dict[str, str]) -> Any:
            tab, name = where.split()
            return pages[tab].evaluate(f"answerOf({json.dumps(name)}, {json.dumps(message)})", timeout=_TIMEOUT_SECONDS)

        everyone: list[str] = []
        for tab, (_, other) in origins.items():
            names = pages[tab].evaluate(f"{_TAB}({json.dumps(other)})", timeout=_TIMEOUT_SECONDS)
            everyone += [f"{tab} {name}" for name in names]
        record: dict[str, dict[str, list[str]]] = {}
        for sender in everyone:
            for where in everyone:
                answer(where, {"act": "heard"})
            answer(sender, {"act": "post", "data": sender})
            answer(sender, {"act": "set", "kind": "localStorage", "key": "from", "value": sender})
            # What Chrome's hear, as far as they come, and then a moment for any other.
            expected = _AS_CHROME["tabs"][sender]
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and not (
                all(answer(where, {"act": "count"})[0] for where in expected["channel"])
                and all(answer(where, {"act": "count"})[1] for where in expected["storage"])
            ):
                time.sleep(0.02)
            time.sleep(0.1)
            heard = {where: answer(where, {"act": "heard"}) for where in everyone}
            record[sender] = {
                "channel": [where for where in everyone if heard[where][0]],
                "storage": [where for where in everyone if heard[where][1]],
            }
        for page in pages.values():
            page.close()

    assert record == _AS_CHROME["tabs"]
