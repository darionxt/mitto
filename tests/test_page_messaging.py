"""Tests of structured clone and messaging in Pages that a Session opened, on local servers.

tests/messaging_cases.js runs in a Page at an HTTP origin and gives what structuredClone(), window.postMessage(),
MessageChannel, MessageEvent and BroadcastChannel did, which tests/data/messaging_cases.json holds as Chrome 157 gave it
for the same script on the same kind of page, headless on macOS (157.0.8103.51), with the page's origin as ORIGIN. The
other tests send BroadcastChannel messages between the Pages of one Session, as Chrome's browser process passes them
between tabs.
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

_TIMEOUT_SECONDS = 60
_BLANK = b"<!doctype html><title>m</title><body>"
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "messaging_cases.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "messaging_cases.json").read_text())

# What a page that waits for a BroadcastChannel message of "news" keeps: the first message's data and origin, or the
# name of its messageerror event.
_LISTEN = """
  globalThis.received = new Promise((resolve) => {
    const channel = new BroadcastChannel('news');
    channel.onmessage = (e) => resolve(['message', e.data, e.origin, e.source, e.isTrusted]);
    channel.onmessageerror = (e) => resolve(['messageerror', e.data, e.origin]);
  });
  globalThis.all = [];
  new BroadcastChannel('news').onmessage = (e) => all.push(e.data);
  'listening'
"""


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


def _serve_blank_page() -> tuple[QuietServer, str]:
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(_BLANK)))
            self.end_headers()
            self.wfile.write(_BLANK)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_port}/"


@pytest.fixture
def blank_page() -> Iterator[str]:
    """The URL of a blank page on 127.0.0.1, which is a secure context."""
    server, url = _serve_blank_page()
    yield url
    server.shutdown()
    server.server_close()


@pytest.fixture
def other_blank_page() -> Iterator[str]:
    """The URL of a blank page of another origin than blank_page's: another port."""
    server, url = _serve_blank_page()
    yield url
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_values_messages_and_their_errors_are_chrome_s(blank_page: str) -> None:
    with mitto.Session() as session, session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES)
    origin = blank_page.rstrip("/")
    as_chrome = json.loads(json.dumps(_AS_CHROME["record"]).replace("ORIGIN", origin))

    assert set(record) == set(as_chrome)
    for section, expected in as_chrome.items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_a_message_for_another_origin_warns_in_the_console_and_fires_nothing(blank_page: str) -> None:
    origin = blank_page.rstrip("/")
    script = """
      new Promise((resolve) => {
        const got = [];
        addEventListener('message', (e) => got.push(e.data));
        postMessage('for another', 'https://example.com/path');
        postMessage('for this one', '/');
        setTimeout(() => resolve(got), 50);
      })
    """
    with mitto.Session() as session, session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script) == ["for this one"]
        warnings = [message.text for message in page.console_messages if "postMessage" in message.text]

    assert warnings == [
        "Failed to execute 'postMessage' on 'DOMWindow': The target origin provided ('https://example.com') does not "
        f"match the recipient window's origin ('{origin}')."
    ]


@pytest.mark.usefixtures("page_program")
def test_a_broadcast_reaches_the_other_pages_of_the_session_and_origin(blank_page: str, other_blank_page: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as sender,  # pyright: ignore[reportPrivateUsage]
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as same_origin,  # pyright: ignore[reportPrivateUsage]
        session._open_page(other_blank_page, timeout=_TIMEOUT_SECONDS) as other_origin,  # pyright: ignore[reportPrivateUsage]
    ):
        assert same_origin.evaluate(_LISTEN) == "listening"
        assert other_origin.evaluate(_LISTEN) == "listening"
        sender.evaluate("new BroadcastChannel('news').postMessage({hello: ['world']}); 'sent'")
        sender.evaluate("new BroadcastChannel('other name').postMessage('elsewhere'); 'sent'")

        assert same_origin.evaluate("received") == [
            "message",
            {"hello": ["world"]},
            blank_page.rstrip("/"),
            None,
            True,
        ]
        assert same_origin.evaluate("new Promise((resolve) => setTimeout(() => resolve(all), 100))") == [
            {"hello": ["world"]}
        ]
        assert other_origin.evaluate("new Promise((resolve) => setTimeout(() => resolve(all), 100))") == []


@pytest.mark.usefixtures("page_program")
def test_a_broadcast_of_a_module_is_a_messageerror_in_another_page(blank_page: str) -> None:
    # A WebAssembly module stays in the agent cluster of the Page that sent it, as a message locked to it.
    with (
        mitto.Session() as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as sender,  # pyright: ignore[reportPrivateUsage]
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as receiver,  # pyright: ignore[reportPrivateUsage]
    ):
        assert receiver.evaluate(_LISTEN) == "listening"
        sender.evaluate(
            "new BroadcastChannel('news').postMessage("
            "new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]))); 'sent'"
        )

        assert receiver.evaluate("received") == ["messageerror", None, blank_page.rstrip("/")]


@pytest.mark.usefixtures("page_program")
def test_pages_of_another_session_hear_no_broadcast(blank_page: str) -> None:
    with (
        mitto.Session() as first,
        mitto.Session() as second,
        first._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as sender,  # pyright: ignore[reportPrivateUsage]
        second._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as elsewhere,  # pyright: ignore[reportPrivateUsage]
    ):
        assert elsewhere.evaluate(_LISTEN) == "listening"
        sender.evaluate("new BroadcastChannel('news').postMessage('only for this Session'); 'sent'")

        assert elsewhere.evaluate("new Promise((resolve) => setTimeout(() => resolve(all), 100))") == []
