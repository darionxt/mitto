"""Tests of window.postMessage() between a Page's documents, on a local server.

tests/frames_messages_cases.js runs in a Page at an HTTP origin, with frames of tests/frames_messages_child.html of its own
origin, of another origin of its site and of another site, and gives what each message says of its sender, whom it reaches
and what it carries. tests/data/frames_messages_cases.json holds what headless Chrome for Testing 157.0.8103.79 gave for the
same scripts, with the page's origin as ORIGIN and the other site's as OTHER.
"""

import http.server
import json
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "frames_messages_cases.js").read_text()
_CHILD = (_TESTS / "frames_messages_child.html").read_bytes()
_AS_CHROME = json.loads((_TESTS / "data" / "frames_messages_cases.json").read_text())


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of an empty page on 127.0.0.1, whose server answers /child?... with tests/frames_messages_child.html, with the
    port of a second such server as its query."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            body = _CHILD if self.path.startswith("/child") else b"<!doctype html><title>t</title><body>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    servers = [QuietServer(("127.0.0.1", 0), Handler) for _ in range(2)]
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{servers[0].server_port}/app/page?{servers[1].server_port}"
    for server in servers:
        server.shutdown()
        server.server_close()


@pytest.mark.usefixtures("page_program")
def test_messages_between_a_page_and_its_frames_are_chrome_s(page_url: str) -> None:
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES, timeout=_TIMEOUT_SECONDS)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_a_message_for_another_origin_warns_in_its_recipient_s_console(page_url: str) -> None:
    # The origins are the one that the sender named, its own for "/", and the recipient's, "null" for an opaque one. Between
    # sites, which Chrome keeps in processes of their own, a message for another origin is dropped without a word.
    script = """
      (async () => {
        const other = `http://localhost:${location.port}`;
        const sameSite = `http://127.0.0.1:${location.search.slice(1)}`;
        const add = async (src) => {
          const element = document.createElement('iframe');
          element.src = src;
          document.body.appendChild(element);
          await new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
          return element;
        };
        postMessage.call((await add('/child')).contentWindow, 'a', 'https://example.com/path');
        postMessage.call((await add(`${sameSite}/child`)).contentWindow, 'b', '/');
        postMessage.call((await add('data:text/html,<p>x')).contentWindow, 'c', location.origin);
        postMessage('d', other);
        postMessage.call((await add(`${other}/child`)).contentWindow, 'e', '/');
        await add(`${other}/child?elsewhere`);
        await add(`${sameSite}/child?elsewhere`);
        await new Promise((resolve) => setTimeout(resolve, 150));
      })()
    """
    with mitto.Session() as session, session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate(script, timeout=_TIMEOUT_SECONDS)
        warnings = [message.text for message in page.console_messages if "postMessage" in message.text]

    origin, _, port = page_url.partition("/app/page?")
    same_site = f"http://127.0.0.1:{port}"
    other = origin.replace("127.0.0.1", "localhost")
    start = "Failed to execute 'postMessage' on 'DOMWindow': The target origin provided"
    assert warnings == [
        f"{start} ('https://example.com') does not match the recipient window's origin ('{origin}').",
        f"{start} ('{origin}') does not match the recipient window's origin ('{same_site}').",
        f"{start} ('{origin}') does not match the recipient window's origin ('null').",
        f"{start} ('{other}') does not match the recipient window's origin ('{origin}').",
        f"{start} ('https://example.com') does not match the recipient window's origin ('{origin}').",
    ]
