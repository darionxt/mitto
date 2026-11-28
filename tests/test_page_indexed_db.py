"""Tests of IndexedDB in a Page that a Session opened, on a local server.

It needs an origin that is not opaque, and so a Page that a Session opened on 127.0.0.1, with a native library and the
Page program beside it. The expected values of tests/indexed_db_cases.js are what Chrome 157 gave for the same script
on the same kind of page, headless on macOS (157.0.8103.51), which tests/data/indexed_db_cases.json keeps, and so are
those of tests/indexed_db_blob_cases.js (157.0.8103.79), which gave the same in a page and in a dedicated worker.
"""

import http.server
import json
import subprocess
import sys
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
_SAVED_STORAGE_CLIENT = _TESTS / "saved_storage_client.py"
# The blob cases, whose value is a promise, and a worker that posts what they settle with.
_BLOB_CASES = (_TESTS / "indexed_db_blob_cases.js").read_text(encoding="utf-8").strip().removesuffix(";")
_BLOB_WORKER_PATH = "/blob-cases.js"
_BLOB_WORKER = f"{_BLOB_CASES}.then(postMessage);".encode()

# Writes a database of two versions, with a store, an index and a key generator, and gives what IndexedDB's part of
# navigator.storage.estimate() says.
_WRITE = """
new Promise((resolve, reject) => {
  const r = indexedDB.open('kept', 2);
  r.onupgradeneeded = () => {
    const s = r.result.createObjectStore('s', {autoIncrement: true});
    s.createIndex('by_name', 'name');
  };
  r.onsuccess = () => {
    const tx = r.result.transaction('s', 'readwrite');
    const s = tx.objectStore('s');
    s.put({name: 'é and 日本', bytes: new Uint8Array([1, 2, 3]),
      file: new File(['of é'], 'kept.txt', {type: 'a/b', lastModified: 9})});
    for (let i = 0; i < 100; ++i) s.put({name: `n${i}`, i});
    tx.oncomplete = async () => {
      r.result.close();
      resolve((await navigator.storage.estimate()).usageDetails.indexedDB);
    };
    tx.onabort = () => reject(tx.error);
  };
  r.onerror = () => reject(r.error);
})
"""
# What a later Session reads of it: what estimate() says before any open, then the database.
_READ = """
(async () => {
  const usage = (await navigator.storage.estimate()).usageDetails.indexedDB;
  const databases = await indexedDB.databases();
  const db = await new Promise((resolve, reject) => {
    const r = indexedDB.open('kept');
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const read = await new Promise((resolve) => {
    const s = db.transaction('s').objectStore('s');
    const first = s.index('by_name').get('é and 日本');
    const count = s.count();
    const last = s.get(101);
    last.onsuccess = () => resolve([count.result, first.result.name, [...first.result.bytes], first.result.file,
      last.result.i]);
  });
  const file = read[3];
  read[3] = [file.name, file.type, file.lastModified, file.size, await file.text()];
  // The key generator goes on from where it was.
  const next = await new Promise((resolve) => {
    const put = db.transaction('s', 'readwrite').objectStore('s').put({name: 'next'});
    put.onsuccess = () => resolve(put.result);
  });
  return [usage, databases, db.version, [...db.objectStoreNames], ...read, next];
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
    """The origin of a local server on 127.0.0.1 with a blank page at every path, but for the blob cases' worker."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            is_worker = self.path == _BLOB_WORKER_PATH
            body = _BLOB_WORKER if is_worker else _BLANK
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript; charset=utf-8" if is_worker else "text/html")
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
def test_indexed_db_stores_reads_and_fails_as_chrome_s(server_origin: str) -> None:
    script = (_TESTS / "indexed_db_cases.js").read_text(encoding="utf-8")
    expected = json.loads((_TESTS / "data" / "indexed_db_cases.json").read_text(encoding="utf-8"))

    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        outcome = page.evaluate(script, timeout=_TIMEOUT_SECONDS)

    assert outcome == expected


@pytest.mark.parametrize(
    "script",
    [
        pytest.param(_BLOB_CASES, id="page"),
        pytest.param(
            f"new Promise((resolve, reject) => {{ const worker = new Worker('{_BLOB_WORKER_PATH}');"
            " worker.onmessage = (e) => resolve(e.data); worker.onerror = (e) => reject(e.message); })",
            id="worker",
        ),
    ],
)
@pytest.mark.usefixtures("page_program")
def test_indexed_db_keeps_blobs_as_chrome_s(server_origin: str, script: str) -> None:
    expected = json.loads((_TESTS / "data" / "indexed_db_blob_cases.json").read_text(encoding="utf-8"))

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
                " r.onupgradeneeded = () => { const s = r.result.createObjectStore('s'); s.put({kept: true}, 'k');"
                " s.put(new File(['of another'], 'f', {type: 'a/b', lastModified: 1}), 'f'); };"
                " r.onsuccess = () => { r.result.close(); resolve(); }; r.onerror = () => reject(r.error); })",
                timeout=_TIMEOUT_SECONDS,
            )
        with session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as reader:  # pyright: ignore[reportPrivateUsage]
            outcome = reader.evaluate(
                "new Promise((resolve, reject) => {"
                " const r = indexedDB.open('shared');"
                " r.onsuccess = () => { const s = r.result.transaction('s').objectStore('s');"
                " const g = s.get('k'); const f = s.get('f');"
                " f.onsuccess = async () => resolve([r.result.version, g.result, f.result.name, f.result.type,"
                " f.result.lastModified, await f.result.text()]); };"
                " r.onerror = () => reject(r.error); })",
                timeout=_TIMEOUT_SECONDS,
            )

    assert outcome == [3, {"kept": True}, "f", "a/b", 1, "of another"]


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
def test_a_version_change_right_after_an_open_reaches_its_new_connection(server_origin: str) -> None:
    # The open's success and the upgrade's versionchange both come while the script is busy: the connection hears
    # the versionchange after its success, closes, and the upgrade goes on.
    script = """(async () => {
        await new Promise((resolve) => {
            const open = indexedDB.open('v', 1);
            open.onsuccess = () => { open.result.close(); resolve(); };
        });
        const first = indexedDB.open('v');
        first.onsuccess = () => { first.result.onversionchange = () => first.result.close(); };
        const second = indexedDB.open('v', 2);
        const outcome = new Promise((resolve) => {
            second.onsuccess = () => resolve('upgraded');
            setTimeout(() => resolve('blocked'), 3000);
        });
        for (const end = Date.now() + 500; Date.now() < end;) {}
        return outcome;
    })()"""

    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == "upgraded"


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


@pytest.mark.usefixtures("page_program")
def test_a_call_too_large_for_a_frame_fails_with_blink_s_error(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        # Values travel whole in a frame of 16 MiB: past that, a call fails as one past Mojo's limit does in Chrome.
        outcome = page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('large', 1);"
            " r.onupgradeneeded = () => r.result.createObjectStore('s');"
            " r.onsuccess = () => { const out = []; const tx = r.result.transaction('s', 'readwrite');"
            " const s = tx.objectStore('s');"
            " const put = s.put(new Uint8Array(17 << 20), 1);"
            " put.onerror = (e) => { e.preventDefault(); out.push([put.error.name, put.error.message.split(' (')[0]]); };"
            " const count = s.count(new Uint8Array(9 << 20));"
            " count.onerror = (e) => { e.preventDefault(); out.push(count.error.name); };"
            " tx.oncomplete = tx.onabort = () => resolve(out); }; })",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == [
        ["UnknownError", "The serialized keys and/or value are too large"],
        "UnknownError",
    ]


@pytest.mark.usefixtures("page_program")
def test_a_name_too_large_for_a_frame_aborts_its_transaction(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        # A store's name of 18 MiB in UTF-16 fits no frame, so the upgrade's transaction aborts and the Page goes on.
        outcome = page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('named', 1);"
            " r.onupgradeneeded = () => r.result.createObjectStore('x'.repeat(9 << 20));"
            " r.onsuccess = () => resolve('success'); r.onerror = () => resolve(r.error.name); })",
            timeout=_TIMEOUT_SECONDS,
        )
        databases = page.evaluate("indexedDB.databases()", timeout=_TIMEOUT_SECONDS)

    assert outcome == "AbortError"
    assert databases == []


@pytest.mark.usefixtures("page_program")
def test_an_open_whose_database_is_too_large_for_a_frame_fails(server_origin: str) -> None:
    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        # Two stores' names of 10 MiB each in UTF-16 fit a frame one at a time, but the database's metadata does not.
        outcome = page.evaluate(
            "(async () => { const log = [];"
            " const open = (version) => new Promise((resolve) => {"
            " const r = version ? indexedDB.open('meta', version) : indexedDB.open('meta');"
            " r.onupgradeneeded = () => { const db = r.result; if (db.objectStoreNames.length) { return; }"
            " db.onclose = () => { log.push('close'); resolve(); };"
            " db.createObjectStore('a'.repeat(5 << 20)); db.createObjectStore('b'.repeat(5 << 20));"
            " r.transaction.oncomplete = () => log.push('complete'); };"
            " r.onsuccess = () => { log.push('success'); r.result.close(); resolve(); };"
            " r.onerror = () => { log.push(`${r.error.name}: ${r.error.message}`); if (version !== 1) { resolve(); } }; });"
            " await open(1); await open(undefined); await open(2);"
            " log.push(await indexedDB.databases()); return log; })()",
            timeout=_TIMEOUT_SECONDS,
        )

    too_large = "UnknownError: The result is too large."
    assert outcome == ["complete", too_large, "close", too_large, too_large, [{"name": "meta", "version": 1}]]


@pytest.mark.usefixtures("page_program")
def test_a_storage_directory_brings_databases_back_in_another_process(server_origin: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        usage = page.evaluate(_WRITE, timeout=_TIMEOUT_SECONDS)

    assert (storage / "IndexedDB").is_file()
    completed = subprocess.run(
        [sys.executable, str(_SAVED_STORAGE_CLIENT), str(storage), f"{server_origin}/", _READ],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS * 2,
        check=True,
    )
    printed = json.loads(completed.stdout)

    assert usage > 0
    assert printed["result"] == [
        usage,
        [{"name": "kept", "version": 2}],
        2,
        ["s"],
        101,
        "é and 日本",
        [1, 2, 3],
        ["kept.txt", "a/b", 9, 5, "of é"],
        99,
        102,
    ]


@pytest.mark.usefixtures("page_program")
def test_the_blobs_of_a_record_that_goes_go_with_it(server_origin: str) -> None:
    script = """(async () => {
        const open = (version, upgrade) => new Promise((resolve) => {
            const r = indexedDB.open('blobs', version);
            r.onupgradeneeded = () => upgrade(r.result);
            r.onsuccess = () => resolve(r.result);
        });
        const usage = async () => ((await navigator.storage.estimate()).usageDetails.indexedDB ?? 0) >> 20;
        let db = await open(1, (d) => { d.createObjectStore('s'); d.createObjectStore('other'); });
        const write = (f) => new Promise((resolve) => {
            const tx = db.transaction('s', 'readwrite');
            f(tx.objectStore('s'));
            tx.oncomplete = async () => resolve(await usage());
        });
        const put = (key) => write((s) => s.put({blob: new Blob([new Uint8Array(1 << 20)])}, key));
        const seen = [await put(1), await put(1), await write((s) => s.put('none', 1)), await put(1),
            await write((s) => s.delete(1)), await put(1), await put(2),
            await write((s) => s.delete(IDBKeyRange.upperBound(1))), await write((s) => s.clear()), await put(3)];
        db.close();
        db = await open(2, (d) => d.deleteObjectStore('s'));
        seen.push(await usage());
        db.close();
        db = await open(3, (d) => d.createObjectStore('s'));
        seen.push(await put(1));
        db.close();
        await new Promise((resolve) => { indexedDB.deleteDatabase('blobs').onsuccess = resolve; });
        seen.push(await usage());
        return seen;
    })()"""

    with mitto.Session() as session, session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == [1, 1, 0, 1, 0, 1, 2, 1, 0, 1, 0, 1, 0]


@pytest.mark.usefixtures("page_program")
def test_a_commit_just_before_close_is_kept(server_origin: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        # The Session closes as soon as the transaction completes, with its connection still open.
        page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('late', 1);"
            " r.onupgradeneeded = () => r.result.createObjectStore('s');"
            " r.onsuccess = () => { const tx = r.result.transaction('s', 'readwrite');"
            " for (let i = 0; i < 1000; ++i) tx.objectStore('s').put('v'.repeat(i), i);"
            " tx.oncomplete = () => resolve(); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        outcome = page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('late');"
            " r.onsuccess = () => { const s = r.result.transaction('s').objectStore('s');"
            " const count = s.count(); const last = s.get(999);"
            " last.onsuccess = () => resolve([count.result, last.result.length]); }; })",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == [1000, 999]


@pytest.mark.usefixtures("page_program")
def test_an_aborted_transaction_leaves_nothing_in_the_storage_directory(server_origin: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('aborted', 1);"
            " r.onupgradeneeded = () => r.result.createObjectStore('s').put('kept', 1);"
            " r.onsuccess = () => { const tx = r.result.transaction('s', 'readwrite');"
            " tx.objectStore('s').put('gone', 2); tx.objectStore('s').delete(1); tx.abort();"
            " tx.onabort = () => resolve(); }; })",
            timeout=_TIMEOUT_SECONDS,
        )
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(f"{server_origin}/", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        outcome = page.evaluate(
            "new Promise((resolve) => { const r = indexedDB.open('aborted');"
            " r.onsuccess = () => { const all = r.result.transaction('s').objectStore('s').getAll();"
            " all.onsuccess = () => resolve(all.result); }; })",
            timeout=_TIMEOUT_SECONDS,
        )

    assert outcome == ["kept"]
