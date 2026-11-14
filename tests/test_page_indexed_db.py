"""Tests of IndexedDB in a Page that a Session opened, on a local server.

It needs an origin that is not opaque, and so a Page that a Session opened on 127.0.0.1, with a native library and the
Page program beside it. The expected values of tests/indexed_db_cases.js are what Chrome 157 gave for the same script
on the same kind of page, headless on macOS (157.0.8103.51), which tests/data/indexed_db_cases.json keeps.
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
_BLANK = b"<!doctype html><title>i</title><body>"
_TESTS = Path(__file__).parent


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server_origin() -> Iterator[str]:
    """The origin of a local server on 127.0.0.1 with a blank page at every path."""

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
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()
    server.server_close()


@pytest.mark.usefixtures("page_program")
def test_indexed_db_stores_reads_and_fails_as_chrome_s(server_origin: str) -> None:
    script = (_TESTS / "indexed_db_cases.js").read_text(encoding="utf-8")
    expected = json.loads((_TESTS / "data" / "indexed_db_cases.json").read_text(encoding="utf-8"))

    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcome = page.evaluate(script, timeout=_TIMEOUT_SECONDS)

    assert outcome == expected


@pytest.mark.usefixtures("page_program")
def test_the_pages_of_a_session_share_its_databases(server_origin: str) -> None:
    with mitto.Session() as session:
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as writer:  # pyright: ignore[reportPrivateUsage]
            writer.evaluate(
                "new Promise((resolve, reject) => {"
                " const r = indexedDB.open('shared', 3);"
                " r.onupgradeneeded = () => r.result.createObjectStore('s').put({kept: true}, 'k');"
                " r.onsuccess = () => { r.result.close(); resolve(); }; r.onerror = () => reject(r.error); })",
                timeout=_TIMEOUT_SECONDS,
            )
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as reader:  # pyright: ignore[reportPrivateUsage]
            outcome = reader.evaluate(
                "new Promise((resolve, reject) => {"
                " const r = indexedDB.open('shared');"
                " r.onsuccess = () => { const g = r.result.transaction('s').objectStore('s').get('k');"
                " g.onsuccess = () => resolve([r.result.version, g.result]); };"
                " r.onerror = () => reject(r.error); })",
                timeout=_TIMEOUT_SECONDS,
            )

    assert outcome == [3, {"kept": True}]


@pytest.mark.usefixtures("page_program")
def test_an_upgrade_in_one_page_asks_another_page_s_connection_to_close(server_origin: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as holder,  # pyright: ignore[reportPrivateUsage]
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as upgrader,  # pyright: ignore[reportPrivateUsage]
    ):
        holder.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('v', 1);"
            " r.onsuccess = () => { window.heard = [];"
            " r.result.onversionchange = (e) => { window.heard.push([e.oldVersion, e.newVersion]);"
            " r.result.close(); }; resolve(); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
        upgraded = upgrader.evaluate(
            "new Promise((resolve) => { const log = []; const r = indexedDB.open('v', 2);"
            " r.onblocked = () => log.push('blocked'); r.onupgradeneeded = (e) => log.push(['upgrade', e.oldVersion]);"
            " r.onsuccess = () => { log.push(['success', r.result.version]); resolve(log); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
        heard = holder.evaluate("window.heard", timeout=_TIMEOUT_SECONDS)

    assert upgraded == [["upgrade", 1], ["success", 2]]
    assert heard == [[1, 2]]


@pytest.mark.usefixtures("page_program")
def test_another_session_has_databases_of_its_own(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate(
            "new Promise((resolve) => { indexedDB.open('mine').onsuccess = (e) => { e.target.result.close();"
            " resolve(); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
        assert page.evaluate("indexedDB.databases()", timeout=_TIMEOUT_SECONDS) == [{"name": "mine", "version": 1}]
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate("indexedDB.databases()", timeout=_TIMEOUT_SECONDS) == []


@pytest.mark.usefixtures("page_program")
def test_a_document_of_an_opaque_origin_may_not_reach_indexed_db(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcome = page.evaluate(
            "(async () => { const r = [String(indexedDB)];"
            " for (const f of [() => indexedDB.open('x'), () => indexedDB.deleteDatabase('x')]) {"
            " try { f(); } catch (e) { r.push(`${e.name}: ${e.message}`); } }"
            " try { await indexedDB.databases(); } catch (e) { r.push(`${e.name}: ${e.message}`); }"
            " return r; })()",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == [
        "[object IDBFactory]",
        "SecurityError: Failed to execute 'open' on 'IDBFactory': access to the Indexed Database API is denied in this"
        " context.",
        "SecurityError: Failed to execute 'deleteDatabase' on 'IDBFactory': access to the Indexed Database API is"
        " denied in this context.",
        "SecurityError: Failed to execute 'databases' on 'IDBFactory': Access to the IndexedDB API is denied in this"
        " context.",
    ]


@pytest.mark.usefixtures("page_program")
def test_a_navigation_closes_the_old_document_s_connections(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        # The old document keeps its connection open, with a transaction, as it navigates.
        page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('nav', 1);"
            " r.onupgradeneeded = () => r.result.createObjectStore('s');"
            " r.onsuccess = () => { r.result.transaction('s', 'readwrite').objectStore('s').put(1, 'k');"
            " location.href = '/next'; resolve(); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
        page.wait_for_navigation(timeout=_TIMEOUT_SECONDS)
        outcome = page.evaluate(
            "new Promise((resolve) => { const log = [location.pathname]; const r = indexedDB.open('nav', 2);"
            " r.onblocked = () => log.push('blocked'); r.onupgradeneeded = (e) => log.push(['upgrade', e.oldVersion]);"
            " r.onsuccess = () => { log.push(['success', r.result.version]); resolve(log); }; })",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == ["/next", ["upgrade", 1], ["success", 2]]
