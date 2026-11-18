"""Tests of dedicated workers in Pages that a Session opened, on a local server.

Each test opens a Page at a blank page of the server and starts workers whose scripts the server holds. What a page sees
of them is held to what Chrome 157 does for the same scripts (docs/runtime.md, "Dedicated workers").
"""

import http.server
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_BLANK = b"<!doctype html><title>w</title><body>"

# The worker scripts that the server holds, by path, each with its Content-Type.
_SCRIPTS: dict[str, tuple[str, bytes]] = {
    "/echo.js": ("text/javascript", b"onmessage = (e) => postMessage([e.data, self.name, typeof document]);"),
    "/hello.js": ("text/javascript", b"postMessage('hello'); postMessage(location === undefined);"),
    "/throws.js": ("text/javascript", b"throw new Error('boom');"),
    "/caught.js": (
        "text/javascript",
        b"onerror = (message, filename, line, column, error) => true; throw new Error('kept');",
    ),
    "/closes.js": (
        "text/javascript",
        b"postMessage('before'); close(); postMessage('after'); setTimeout(() => postMessage('timer'));",
    ),
    "/html.js": ("text/html", b"postMessage('ran as text/html');"),
    "/image.js": ("image/png", b"postMessage('ran as an image');"),
    "/nested.js": (
        "text/javascript",
        b"const inner = new Worker('/echo.js', {name: 'inner'});"
        b"inner.onmessage = (e) => postMessage(['from inner', e.data]); inner.postMessage('ping');",
    ),
    "/lib.js": ("text/javascript", b"var fromLib = (fromLib || 0) + 1;"),
    "/lib.txt": ("text/plain", b"var fromText = true;"),
    "/lib-throws.js": ("text/javascript", b"throw new TypeError('in lib');"),
    "/imports.js": (
        "text/javascript",
        b"var fromLib; importScripts('/lib.js', 'lib.js'); importScripts();"
        b"const outcome = (run) => { try { run(); return 'ok'; } catch (e) { return String(e); } };"
        b"postMessage([fromLib, outcome(() => importScripts('/missing.js')), outcome(() => importScripts('/lib.txt')),"
        b"outcome(() => importScripts('http://[')), outcome(() => importScripts('/lib-throws.js'))]);",
    ),
    "/where.js": (
        "text/javascript",
        b"postMessage([location.href, location.pathname, String(location), location.origin, self.origin,"
        b"isSecureContext, navigator.userAgent, navigator.platform, navigator.language, navigator.hardwareConcurrency,"
        b"navigator.onLine, Object.prototype.toString.call(navigator), typeof navigator.vendor]);",
    ),
    "/fetches.js": (
        "text/javascript",
        b"fetch('/data.txt').then((r) => r.text()).then((t) => postMessage([t, performance.now() > 0]));",
    ),
    "/data.txt": ("text/plain", b"data from the server"),
    "/broadcast.js": (
        "text/javascript",
        b"const channel = new BroadcastChannel('news');"
        b"channel.onmessage = (e) => channel.postMessage(['worker heard', e.data, e.origin]);"
        b"postMessage('listening');",
    ),
    "/apis.js": (
        "text/javascript",
        b"Promise.all([navigator.userAgentData.getHighEntropyValues(['platform']).then((v) => v.platform),"
        b"scheduler.postTask(() => 'posted'), Promise.resolve(crypto.getRandomValues(new Uint8Array(4)).length),"
        b"navigator.permissions.query({name: 'storage-access'}).then((s) => s.state),"
        b"Promise.resolve(Notification.permission), Promise.resolve(PerformanceObserver.supportedEntryTypes.join())])"
        b".then((values) => postMessage(values));",
    ),
    "/data-imports.js": (
        "text/javascript",
        b"var x = 'a', ran = false; const outcome = (run) => { try { run(); return 'ok'; } catch (e) { return e; } };"
        b"importScripts('data:text/javascript,x+=\"b\"', 'data:text/javascript,x+=\"c\"');"
        b"const invalid = String(outcome(() => importScripts('data:text/javascript,ran=true', 'http://[')));"
        b"const thrown = outcome(() => importScripts('data:text/javascript,throw 2', 'data:text/javascript,x+=\"d\"'));"
        b"postMessage([x, ran, invalid, thrown]);",
    ),
    "/cross-origin-imports.js": (
        "text/javascript",
        b"const outcome = (run) => { try { run(); return 'ok'; } catch (e) { return String(e); } };"
        b"const other = location.origin.replace('127.0.0.1', 'localhost');"
        b"postMessage([outcome(() => importScripts(other + '/lib-throws.js')), outcome(() => importScripts(other + '/lib.js')),"
        b"typeof fromLib]);",
    ),
    "/timeline.js": (
        "text/javascript",
        b"performance.mark('m'); postMessage([performance.getEntriesByName('m').length,"
        b"performance.getEntriesByType('navigation').length, typeof performance.timing]);",
    ),
    "/locks.js": (
        "text/javascript",
        b"onmessage = async (e) => {"
        b"  if (e.data === 'try') {"
        b"    postMessage(await navigator.locks.request('x', {ifAvailable: true}, (lock) => (lock ? 'free' : 'busy')));"
        b"  } else {"
        b"    navigator.locks.request('y', () => { postMessage('holding'); return new Promise(() => {}); });"
        b"  }"
        b"};",
    ),
    "/caches.js": (
        "text/javascript",
        b"(async () => {"
        b"  const cache = await caches.open('shared');"
        b"  await cache.put('/data.txt', new Response('put by the worker'));"
        b"  const keys = await caches.keys();"
        b"  const text = await (await caches.match('/data.txt')).text();"
        b"  const estimate = await navigator.storage.estimate();"
        b"  postMessage([keys, text, estimate.usageDetails.caches > 0]);"
        b"})().catch((e) => postMessage(String(e)));",
    ),
    "/indexed-db.js": (
        "text/javascript",
        b"const open = indexedDB.open('shared', 1);"
        b"open.onupgradeneeded = () => open.result.createObjectStore('things');"
        b"open.onsuccess = () => {"
        b"  const database = open.result;"
        b"  const transaction = database.transaction('things', 'readwrite');"
        b"  transaction.objectStore('things').put({by: 'the worker'}, 'key');"
        b"  transaction.oncomplete = async () => {"
        b"    database.close();"
        b"    postMessage([(await indexedDB.databases()).map((info) => [info.name, info.version])]);"
        b"  };"
        b"};"
        b"open.onerror = () => postMessage(String(open.error));",
    ),
    "/port.js": (
        "text/javascript",
        b"onmessage = (e) => { const port = e.ports[0]; port.onmessage = (m) => port.postMessage(m.data + '!'); };",
    ),
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server() -> Iterator[str]:
    """The origin of a server on 127.0.0.1, which is a secure context, with a blank page at / and the worker scripts."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            path = self.path.partition("?")[0]
            if path in _SCRIPTS:
                content_type, body = _SCRIPTS[path]
                status = 200
            elif path == "/":
                content_type, body, status = "text/html", _BLANK, 200
            else:
                content_type, body, status = "text/plain", b"not found", 404
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()
    server.server_close()


def _evaluate(origin: str, script: str) -> Any:
    with mitto.Session() as session, session._open_page(f"{origin}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        return page.evaluate(script)


@pytest.mark.usefixtures("page_program")
def test_a_worker_and_its_page_message_each_other(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/echo.js', {name: 'echo'});
        worker.onmessage = (e) => resolve([e.data, e.origin, e.source, e.isTrusted, e.ports.length]);
        worker.postMessage({n: 21});
    })"""

    assert _evaluate(server, script) == [[{"n": 21}, "echo", "undefined"], "", None, True, 0]


@pytest.mark.usefixtures("page_program")
def test_a_worker_posts_before_it_is_messaged(server: str) -> None:
    script = """new Promise((resolve) => {
        const seen = [];
        const worker = new Worker('/hello.js');
        worker.onmessage = (e) => { seen.push(e.data); if (seen.length === 2) resolve(seen); };
    })"""

    assert _evaluate(server, script) == ["hello", False]


@pytest.mark.usefixtures("page_program")
def test_an_uncaught_error_reaches_the_worker_and_then_the_window(server: str) -> None:
    # As in Chrome: a new ErrorEvent at the Worker, with a null error, and the same event at the window.
    script = """new Promise((resolve) => {
        const seen = [];
        window.addEventListener('error', (e) => {
            seen.push(['window', e.message, e.filename.replace(location.origin, ''), e.lineno, e.colno, e.error]);
            e.preventDefault();
            resolve(seen);
        });
        const worker = new Worker('/throws.js');
        worker.onerror = (e) => {
            seen.push(['worker', e.constructor.name, e.message, e.filename.replace(location.origin, ''), e.lineno,
                       e.colno, e.error, e.cancelable, e.bubbles]);
        };
    })"""

    assert _evaluate(server, script) == [
        ["worker", "ErrorEvent", "Uncaught Error: boom", "/throws.js", 1, 1, None, True, False],
        ["window", "Uncaught Error: boom", "/throws.js", 1, 1, None],
    ]


@pytest.mark.usefixtures("page_program")
def test_an_error_that_the_worker_cancels_stays_there(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/caught.js');
        worker.onerror = () => resolve('reached the Worker');
        setTimeout(() => resolve('stayed in the worker'), 500);
    })"""

    assert _evaluate(server, script) == "stayed in the worker"


@pytest.mark.usefixtures("page_program")
def test_a_worker_that_closes_ends_after_its_task(server: str) -> None:
    script = """new Promise((resolve) => {
        const seen = [];
        const worker = new Worker('/closes.js');
        worker.onmessage = (e) => seen.push(e.data);
        setTimeout(() => resolve(seen), 500);
    })"""

    assert _evaluate(server, script) == ["before", "after"]


@pytest.mark.usefixtures("page_program")
def test_a_terminated_worker_sends_nothing_more(server: str) -> None:
    script = """new Promise((resolve) => {
        const seen = [];
        const worker = new Worker('/echo.js');
        worker.onmessage = (e) => seen.push(e.data);
        worker.terminate();
        worker.postMessage('lost');
        setTimeout(() => resolve(seen), 500);
    })"""

    assert _evaluate(server, script) == []


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize(
    ("path", "outcome"),
    [
        # A window's worker is held to the lax check of its MIME type.
        ("/html.js", "ran as text/html"),
        ("/image.js", "error Event"),
        ("/missing.js", "error Event"),
    ],
)
def test_a_script_that_is_not_one_to_run_fires_a_plain_error(server: str, path: str, outcome: str) -> None:
    script = f"""new Promise((resolve) => {{
        const worker = new Worker('{path}');
        worker.onmessage = (e) => resolve(e.data);
        worker.onerror = (e) => resolve(e.constructor.name === 'Event' && e.cancelable ? 'error Event' : 'other');
    }})"""

    assert _evaluate(server, script) == outcome


@pytest.mark.usefixtures("page_program")
def test_a_worker_starts_its_own_worker(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/nested.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == ["from inner", ["ping", "inner", "undefined"]]


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize(
    ("url", "thrown"),
    [
        ("http://[", "SyntaxError: Failed to construct 'Worker': 'http://[' is not a valid URL."),
        (
            "https://example.com/w.js",
            "SecurityError: Failed to construct 'Worker': Script at 'https://example.com/w.js' cannot be accessed "
            "from origin 'ORIGIN'.",
        ),
    ],
)
def test_new_worker_checks_its_url(server: str, url: str, thrown: str) -> None:
    script = f"(() => {{ try {{ new Worker('{url}'); return 'no throw'; }} catch (e) {{ return String(e); }} }})()"

    assert _evaluate(server, script) == thrown.replace("ORIGIN", server)


@pytest.mark.usefixtures("page_program")
def test_import_scripts_runs_scripts_in_order_and_fails_as_in_chrome(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/imports.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    failed = f"NetworkError: Failed to execute 'importScripts' on 'WorkerGlobalScope': The script at '{server}"
    assert _evaluate(server, script) == [
        2,
        f"{failed}/missing.js' failed to load.",
        # importScripts() holds a script to the strict check of its MIME type.
        f"{failed}/lib.txt' failed to load.",
        "SyntaxError: Failed to execute 'importScripts' on 'WorkerGlobalScope': The URL 'http://[' is invalid.",
        "TypeError: in lib",
    ]


@pytest.mark.usefixtures("page_program")
def test_import_scripts_runs_data_urls_and_checks_every_url_first(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/data-imports.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    # What a data: URL's script throws is not muted, and the scripts after it do not run.
    assert _evaluate(server, script) == [
        "abc",
        False,
        "SyntaxError: Failed to execute 'importScripts' on 'WorkerGlobalScope': The URL 'http://[' is invalid.",
        2,
    ]


@pytest.mark.usefixtures("page_program")
def test_import_scripts_mutes_what_a_script_of_another_origin_throws(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/cross-origin-imports.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    other = server.replace("127.0.0.1", "localhost")
    assert _evaluate(server, script) == [
        "NetworkError: Failed to execute 'importScripts' on 'WorkerGlobalScope': "
        f"The script at '{other}/lib-throws.js' failed to load.",
        "ok",
        "number",
    ]


@pytest.mark.usefixtures("page_program")
def test_a_worker_of_a_data_url_has_an_opaque_origin(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('data:text/javascript,postMessage([self.origin, location.href])');
        worker.onerror = () => resolve('error');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == ["null", "data:text/javascript,postMessage([self.origin, location.href])"]


@pytest.mark.usefixtures("page_program")
def test_a_workers_performance_timeline_has_no_navigation(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/timeline.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == [1, 0, "undefined"]


@pytest.mark.usefixtures("page_program")
def test_a_worker_reads_its_location_and_its_creators_navigator(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/where.js?q#f');
        worker.onerror = () => resolve('error');
        worker.onmessage = (e) => resolve([e.data, [navigator.userAgent, navigator.platform, navigator.language,
                                                    navigator.hardwareConcurrency]]);
    })"""

    worker, window = _evaluate(server, script)
    assert worker[:6] == [f"{server}/where.js?q#f", "/where.js", f"{server}/where.js?q#f", server, server, True]
    assert worker[6:10] == window
    assert worker[10:] == [True, "[object WorkerNavigator]", "undefined"]


@pytest.mark.usefixtures("page_program")
def test_a_worker_fetches_through_the_session(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/fetches.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == ["data from the server", True]


@pytest.mark.usefixtures("page_program")
def test_a_port_transferred_to_a_worker_carries_messages_both_ways(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/port.js');
        const channel = new MessageChannel();
        channel.port1.onmessage = (e) => resolve(e.data);
        worker.postMessage('port', [channel.port2]);
        channel.port1.postMessage('across threads');
    })"""

    assert _evaluate(server, script) == "across threads!"


@pytest.mark.usefixtures("page_program")
def test_a_window_and_its_worker_share_their_origins_web_locks(server: str) -> None:
    script = """(async () => {
        const worker = new Worker('/locks.js');
        const answer = (message) => new Promise((resolve) => {
            worker.addEventListener('message', (e) => resolve(e.data), {once: true});
            worker.postMessage(message);
        });
        const out = [];
        let release;
        const held = navigator.locks.request('x', () => new Promise((resolve) => { release = resolve; }));
        await navigator.locks.query();
        out.push(await answer('try'));
        release();
        await held;
        // Answered once the release has reached the service, from the same queue.
        await navigator.locks.query();
        out.push(await answer('try'));
        out.push(await answer('hold'));
        out.push((await navigator.locks.query()).held.map((lock) => lock.name));
        // A worker that ends lets its locks go.
        worker.terminate();
        out.push(await navigator.locks.request('y', () => 'granted after terminate()'));
        return out;
    })()"""

    assert _evaluate(server, script) == ["busy", "free", "holding", ["y"], "granted after terminate()"]


@pytest.mark.usefixtures("page_program")
def test_a_worker_reaches_the_caches_of_its_origin(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/caches.js');
        worker.onmessage = async (e) => {
            const response = await caches.match('/data.txt', {cacheName: 'shared'});
            resolve([e.data, await response.text()]);
        };
    })"""

    assert _evaluate(server, script) == [[["shared"], "put by the worker", True], "put by the worker"]


@pytest.mark.usefixtures("page_program")
def test_a_worker_reaches_the_databases_of_its_origin(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/indexed-db.js');
        worker.onmessage = (e) => {
            const open = indexedDB.open('shared');
            open.onsuccess = () => {
                const get = open.result.transaction('things').objectStore('things').get('key');
                get.onsuccess = () => resolve([e.data, get.result]);
            };
        };
    })"""

    assert _evaluate(server, script) == [[[["shared", 1]]], {"by": "the worker"}]


@pytest.mark.usefixtures("page_program")
def test_a_broadcast_goes_between_a_window_and_its_worker(server: str) -> None:
    script = """new Promise((resolve) => {
        const channel = new BroadcastChannel('news');
        channel.onmessage = (e) => resolve([e.data, e.origin === location.origin]);
        const worker = new Worker('/broadcast.js');
        worker.onmessage = () => channel.postMessage('from the window');
    })"""

    assert _evaluate(server, script) == [["worker heard", "from the window", server], True]


@pytest.mark.usefixtures("page_program")
def test_a_workers_apis_answer_as_chromes_do_in_a_worker(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/apis.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    # Chrome's browser answers for a context with no frame.
    assert _evaluate(server, script) == ["Windows", "posted", 4, "prompt", "denied", "mark,measure,resource"]
