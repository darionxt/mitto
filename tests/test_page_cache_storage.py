"""Tests of the Cache API in a Page that a Session opened, on a local server.

It needs an origin that is not opaque and a secure context, and so a Page that a Session opened on 127.0.0.1, with a
native library and the Page program beside it. The expected values of tests/cache_storage_cases.js are what Chrome 157
gave for the same script on the same kind of page, headless on macOS (157.0.8103.51), which
tests/data/cache_storage_cases.json keeps.
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
_BLANK = b"<!doctype html><title>c</title><body>"
_TESTS = Path(__file__).parent

# What the server answers for /res/<name>: a status, header lines and a body. Any other name gives a 200 of
# "body <name>".
_RESOURCES: dict[str, tuple[int, list[tuple[str, str]], bytes]] = {
    "a": (200, [("Content-Type", "text/plain"), ("X-A", "1")], b"body a"),
    "missing": (404, [("Content-Type", "text/plain")], b"no"),
    "vary-star": (200, [("Vary", "*")], b"star"),
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server_origin() -> Iterator[str]:
    """The origin of a local server on 127.0.0.1, which is a secure context, with a blank page at / and resources at
    /res/<name>."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            path = self.path.split("?")[0]
            if path.startswith("/res/"):
                name = path.removeprefix("/res/")
                status, headers, body = _RESOURCES.get(
                    name, (200, [("Content-Type", "text/plain")], b"body " + name.encode())
                )
            else:
                status, headers, body = 200, [("Content-Type", "text/html")], _BLANK
            self.send_response(status)
            for name, value in headers:
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_the_cache_api_stores_matches_and_fails_as_chrome_s(server_origin: str) -> None:
    script = (_TESTS / "cache_storage_cases.js").read_text()
    expected = json.loads((_TESTS / "data" / "cache_storage_cases.json").read_text())

    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcome = page.evaluate(script, timeout=_TIMEOUT_SECONDS)

    assert outcome == expected


@pytest.mark.usefixtures("page_program")
def test_the_pages_of_a_session_share_its_caches(server_origin: str) -> None:
    with mitto.Session() as session:
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as writer:  # pyright: ignore[reportPrivateUsage]
            writer.evaluate(
                "(async () => { const c = await caches.open('shared');"
                " await c.put('/res/x', new Response('kept', {headers: {'X-Kept': '1'}})); })()",
                timeout=_TIMEOUT_SECONDS,
            )
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as reader:  # pyright: ignore[reportPrivateUsage]
            outcome = reader.evaluate(
                "(async () => { const r = await caches.match('/res/x');"
                " return [await caches.keys(), r.headers.get('x-kept'), await r.text()]; })()",
                timeout=_TIMEOUT_SECONDS,
            )

    assert outcome == [["shared"], "1", "kept"]


@pytest.mark.usefixtures("page_program")
def test_another_session_has_caches_of_its_own(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate("caches.open('mine')", timeout=_TIMEOUT_SECONDS)
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate("caches.keys()", timeout=_TIMEOUT_SECONDS) == []


@pytest.mark.usefixtures("page_program")
def test_a_document_of_an_opaque_origin_may_not_reach_caches(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcome = page.evaluate(
            "(() => { try { return String(caches); } catch (e) { return `${e.name}: ${e.message}`; } })()",
            timeout=_TIMEOUT_SECONDS,
        )

    assert (
        outcome
        == "SecurityError: Failed to read the 'caches' property from 'Window': Access to cache storage is denied."
    )


@pytest.mark.usefixtures("page_program")
def test_a_session_closes_while_a_put_waits_for_its_body(server_origin: str) -> None:
    def put_and_close() -> None:
        with mitto.Session() as session:
            # The Page stays open: the Session ends it as it closes.
            page = session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]
            # The put sends its 128 MiB a megabyte at a time, as the Session asks. The Session answers has() once it
            # has the put, whose body is still on its way when the Session closes.
            page.evaluate(
                "(async () => { const c = await caches.open('big');"
                " c.put('/res/big', new Response(new Uint8Array(128 << 20)));"
                " await new Promise((resolve) => setTimeout(resolve, 0)); await caches.has('big'); })()",
                timeout=_TIMEOUT_SECONDS,
            )

    closer = threading.Thread(target=put_and_close, daemon=True)
    closer.start()
    closer.join(timeout=_TIMEOUT_SECONDS)

    assert not closer.is_alive()


@pytest.mark.usefixtures("page_program")
def test_a_call_too_large_for_a_frame_fails_without_ending_the_page(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        # The call takes 16,777,215 bytes, which fit a frame only without the ID of the document that asks.
        outcome = page.evaluate(
            "caches.open('x'.repeat(8388601)).then(() => 'opened', (e) => e.name)",
            timeout=_TIMEOUT_SECONDS,
        )
        alive = page.evaluate("1 + 1", timeout=_TIMEOUT_SECONDS)

    assert outcome != "opened"
    assert alive == 2
