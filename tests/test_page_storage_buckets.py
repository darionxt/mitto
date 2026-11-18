"""Tests of the Storage Buckets API in a Page that a Session opened, on a local server.

It needs a secure context of an origin that is not opaque, and so a Page that a Session opened on 127.0.0.1, with a
native library and the Page program beside it. The expected values of tests/storage_buckets_cases.js are what Chrome 157
gave for the same script on the same kind of page, headless on macOS (157.0.8103.79) in a profile of its own, which
tests/data/storage_buckets_cases.json keeps.
"""

import http.server
import json
import sqlite3
import subprocess
import sys
import threading
from collections.abc import Iterator
from contextlib import closing
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_BLANK = b"<!doctype html><title>i</title><body>"
_TESTS = Path(__file__).parent
_SAVED_STORAGE_CLIENT = _TESTS / "saved_storage_client.py"

# Opens two buckets, one with policies, and writes a database and a cache in the first.
_WRITE = """
(async () => {
  const kept = await navigator.storageBuckets.open('kept', {quota: 5000000, expires: 4e12});
  await navigator.storageBuckets.open('empty');
  const db = await new Promise((resolve, reject) => {
    const r = kept.indexedDB.open('d', 3);
    r.onupgradeneeded = () => r.result.createObjectStore('s').put({mail: 'é and 日本'}, 'k');
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  db.close();
  await (await kept.caches.open('c')).put('/a', new Response('bread'));
  return (await kept.estimate()).usage;
})()
"""
# What a later Session reads of them.
_READ = """
(async () => {
  const keys = await navigator.storageBuckets.keys();
  const kept = await navigator.storageBuckets.open('kept');
  const estimate = await kept.estimate();
  const db = await new Promise((resolve, reject) => {
    const r = kept.indexedDB.open('d');
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const value = await new Promise((resolve) => {
    const g = db.transaction('s').objectStore('s').get('k');
    g.onsuccess = () => resolve(g.result);
  });
  const body = await (await (await kept.caches.open('c')).match('/a')).text();
  return [keys, estimate.usage, estimate.quota, await kept.expires(), db.version, value, body,
    await indexedDB.databases(), await caches.keys()];
})()
"""


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
@pytest.mark.parametrize("kept", [False, True], ids=["in memory", "in a directory"])
def test_storage_buckets_open_store_and_go_as_chrome_s(server_origin: str, tmp_path: Path, kept: bool) -> None:
    script = (_TESTS / "storage_buckets_cases.js").read_text(encoding="utf-8")
    expected = json.loads((_TESTS / "data" / "storage_buckets_cases.json").read_text(encoding="utf-8"))
    storage = tmp_path / "storage" if kept else None

    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        outcome = page.evaluate(script, timeout=_TIMEOUT_SECONDS)
        known_gaps = page.known_gaps

    assert outcome == expected
    # A bucket's getDirectory() answers as navigator.storage's does.
    assert known_gaps == ["StorageBucket.prototype.getDirectory"]


@pytest.mark.usefixtures("page_program")
def test_the_pages_of_a_session_share_its_buckets(server_origin: str) -> None:
    with mitto.Session() as session:
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as writer:  # pyright: ignore[reportPrivateUsage]
            writer.evaluate(
                "(async () => { const b = await navigator.storageBuckets.open('shared');"
                " await (await b.caches.open('c')).put('/a', new Response('A')); })()",
                timeout=_TIMEOUT_SECONDS,
            )
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as reader:  # pyright: ignore[reportPrivateUsage]
            outcome = reader.evaluate(
                "(async () => { const keys = await navigator.storageBuckets.keys();"
                " const b = await navigator.storageBuckets.open('shared');"
                " return [keys, await (await (await b.caches.open('c')).match('/a')).text()]; })()",
                timeout=_TIMEOUT_SECONDS,
            )

    assert outcome == [["shared"], "A"]


@pytest.mark.usefixtures("page_program")
def test_a_bucket_deleted_in_one_page_closes_another_page_s_connection(server_origin: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as holder,  # pyright: ignore[reportPrivateUsage]
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as deleter,  # pyright: ignore[reportPrivateUsage]
    ):
        holder.evaluate(
            "(async () => { const b = await navigator.storageBuckets.open('gone'); window.heard = [];"
            " await new Promise((resolve) => { const r = b.indexedDB.open('d');"
            " r.onsuccess = () => { r.result.onclose = () => window.heard.push('close'); resolve(); }; }); })()",
            timeout=_TIMEOUT_SECONDS,
        )
        deleter.evaluate(
            "navigator.storageBuckets.delete('gone').then(() => navigator.storageBuckets.keys())",
            timeout=_TIMEOUT_SECONDS,
        )
        heard = holder.evaluate(
            "new Promise((resolve) => setTimeout(() => resolve(window.heard), 500))", timeout=_TIMEOUT_SECONDS
        )

    assert heard == ["close"]


@pytest.mark.usefixtures("page_program")
def test_another_session_has_buckets_of_its_own(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate("navigator.storageBuckets.open('mine')", timeout=_TIMEOUT_SECONDS)
        assert page.evaluate("navigator.storageBuckets.keys()", timeout=_TIMEOUT_SECONDS) == ["mine"]
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate("navigator.storageBuckets.keys()", timeout=_TIMEOUT_SECONDS) == []


@pytest.mark.usefixtures("page_program")
def test_a_page_that_is_not_a_secure_context_has_no_buckets(server_origin: str) -> None:
    # http://www.example.test/ through the local server as a proxy, a URL that is not potentially trustworthy.
    with (
        mitto.Session(proxy=server_origin) as session,
        session._open_page("http://www.example.test/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        outcome = page.evaluate(
            "[isSecureContext, typeof navigator.storageBuckets, 'StorageBucketManager' in self, 'StorageBucket' in self]",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == [False, "undefined", False, False]


@pytest.mark.usefixtures("page_program")
def test_a_storage_directory_brings_buckets_back_in_another_process(server_origin: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        usage = page.evaluate(_WRITE, timeout=_TIMEOUT_SECONDS)

    assert (storage / "QuotaManager").is_file()
    completed = subprocess.run(
        [sys.executable, str(_SAVED_STORAGE_CLIENT), str(storage), f"{server_origin}/", _READ],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS * 2,
        check=True,
    )
    printed = json.loads(completed.stdout)

    assert usage > 0
    # The bucket keeps its policies, its database and its cache, apart from the default bucket's.
    assert printed["result"] == [["empty", "kept"], usage, 5000000, 4e12, 3, {"mail": "é and 日本"}, "bread", [], []]


@pytest.mark.usefixtures("page_program")
def test_a_deleted_bucket_leaves_nothing_in_the_storage_directory(server_origin: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        page.evaluate(_WRITE, timeout=_TIMEOUT_SECONDS)
        page.evaluate(
            "(async () => { await navigator.storageBuckets.delete('kept');"
            " await navigator.storageBuckets.delete('empty'); })()",
            timeout=_TIMEOUT_SECONDS,
        )

    with closing(sqlite3.connect(storage / "QuotaManager")) as database:
        assert database.execute("SELECT name FROM buckets").fetchall() == []
    with closing(sqlite3.connect(storage / "IndexedDB")) as database:
        assert database.execute("SELECT COUNT(*) FROM entries").fetchone() == (0,)
    assert list((storage / "CacheStorage").iterdir()) == []
