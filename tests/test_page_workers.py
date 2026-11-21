"""Tests of dedicated workers in Pages that a Session opened, on a local server.

Each test opens a Page at a blank page of the server and starts workers whose scripts the server holds. What a page sees
of them is held to what Chrome 157 does for the same scripts (docs/runtime.md, "Dedicated workers").
"""

import http.server
import threading
import time
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
        b"fetch('/data.txt').then((r) => r.text()).then((t) => postMessage([t, performance.now() > 0,"
        b"performance.getEntriesByType('resource').map((e) => [e.name.replace(location.origin, ''), e.initiatorType])]));",
    ),
    "/data.txt": ("text/plain", b"data from the server"),
    # A fetch() whose body is a stream, which goes only over HTTP/2 or HTTP/3, so over this server's HTTP/1.1 it fails
    # with the stream read once and locked, as a window's does in Chrome.
    "/uploads.js": (
        "text/javascript",
        b"let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; c.enqueue(new Uint8Array([1])); } });"
        b" fetch('/data.txt', { method: 'POST', duplex: 'half', body }).then(() => 'fulfilled', (e) => 'rejected ' + e)"
        b".then((s) => setTimeout(() => postMessage([s, pulls, body.locked].join(' / ')), 0));",
    ),
    "/broadcast.js": (
        "text/javascript",
        b"const channel = new BroadcastChannel('news');"
        b"channel.onmessage = (e) => channel.postMessage(['worker heard', e.data, e.origin]);"
        b"postMessage('listening');",
    ),
    # A worker that posts to a channel and closes in the same task.
    "/broadcast-close.js": (
        "text/javascript",
        b"new BroadcastChannel('news').postMessage('last words'); close();",
    ),
    # A worker that fetches without end, with a worker of its own that does too, and that is busy for a while from
    # when it says so, past its creator's document being replaced, before it fetches again.
    "/busy.js": (
        "text/javascript",
        b"const go = () => fetch('/data.txt').then(go, go); go(); new Worker('/busy-child.js');"
        b" postMessage('busy');"
        b" setTimeout(() => { const end = Date.now() + 1500; while (Date.now() < end) {} go(); }, 0);",
    ),
    "/busy-child.js": ("text/javascript", b"const go = () => fetch('/data.txt').then(go, go); go();"),
    # A worker that is busy for a while from when it says so, past its creator's document being replaced, before it
    # asks the Cache API and puts there.
    "/busy-caches.js": (
        "text/javascript",
        b"postMessage('busy'); setTimeout(() => { const end = Date.now() + 1500; while (Date.now() < end) {}"
        b" caches.keys(); caches.open('late').then((c) => c.put('/late', new Response('late'))); }, 0);",
    ),
    # The same, before it opens a database and asks for its storage buckets.
    "/busy-storage.js": (
        "text/javascript",
        b"postMessage('busy'); setTimeout(() => { const end = Date.now() + 1500; while (Date.now() < end) {}"
        b" indexedDB.open('late'); navigator.storageBuckets.keys(); }, 0);",
    ),
    # performance.measure() from a name of performance.timing, which a worker has not, as Chrome 157.0.8103.79 throws.
    "/measure.js": (
        "text/javascript",
        b"const out = []; for (const args of [['navigationStart'], ['nope'], [{start: 'loadEventEnd'}],"
        b" [undefined, 'navigationStart']]) { try { performance.measure('m', ...args); out.push('ok'); }"
        b" catch (e) { out.push(e.name + ': ' + e.message); } } postMessage(out);",
    ),
    # The Persona's high-entropy client hints, which settle from a task of the worker's.
    "/ua-data.js": (
        "text/javascript",
        b"navigator.userAgentData.getHighEntropyValues(['architecture', 'platformVersion', 'fullVersionList'])"
        b".then((v) => postMessage(v), (e) => postMessage(String(e)));",
    ),
    # Marks named as attributes of performance.timing, which a worker may make and measure from.
    "/timing-marks.js": (
        "text/javascript",
        b"const out = []; const t = (f) => { try { out.push(String(f())); } catch (e) { out.push(e.name + ': ' +"
        b" e.message); } }; t(() => performance.mark('navigationStart').name);"
        b" t(() => new PerformanceMark('loadEventEnd').name);"
        b" t(() => typeof performance.measure('m', 'navigationStart').duration);"
        b" t(() => performance.getEntriesByName('navigationStart').length); postMessage(out);",
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
    # A worker that closes in the callback of its lock, which lets the lock go as the callback returns.
    "/lock-close.js": (
        "text/javascript",
        b"navigator.locks.request('x', () => { postMessage('got'); close(); });",
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
    # A worker that holds a cache and a matched response that it has not read.
    "/hold-cache.js": (
        "text/javascript",
        b"(async () => {"
        b"  const cache = await caches.open('held');"
        b"  await cache.put('/held', new Response('x'.repeat(1 << 20)));"
        b"  self.kept = [cache, await cache.match('/held')];"
        b"  postMessage('holding');"
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
    "/frames.js": (
        "text/javascript",
        b"const times = []; let cancelledRan = false;"
        b"cancelAnimationFrame(requestAnimationFrame(() => { cancelledRan = true; }));"
        b"const step = (time) => {"
        b"  times.push(time);"
        b"  if (times.length < 3) { requestAnimationFrame(step); return; }"
        b"  postMessage([times[0] < times[1] && times[1] < times[2], times[2] <= performance.now(), cancelledRan]);"
        b"};"
        b"requestAnimationFrame(step);",
    ),
    "/nested-frames.js": (
        "text/javascript",
        b"const inner = new Worker('/no-frames.js'); inner.onmessage = (e) => postMessage(e.data);",
    ),
    "/no-frames.js": (
        "text/javascript",
        b"try { requestAnimationFrame(() => {}); postMessage('no error'); } catch (e) { postMessage(String(e)); }",
    ),
    "/activation.js": (
        "text/javascript",
        b"onmessage = (e) => {"
        b"  const channel = new MessageChannel();"
        b"  channel.port2.onmessage = (m) => postMessage([e.userActivation && e.userActivation.hasBeenActive,"
        b"    m.userActivation]);"
        b"  channel.port1.postMessage(null, {includeUserActivation: true});"
        b"};",
    ),
    "/buckets.js": (
        "text/javascript",
        b"(async () => {"
        b"  const bucket = await navigator.storageBuckets.open('drafts');"
        b"  const cache = await bucket.caches.open('in-a-bucket');"
        b"  await cache.put('/data.txt', new Response('in the bucket'));"
        b"  postMessage([bucket.name, await navigator.storageBuckets.keys()]);"
        b"})().catch((e) => postMessage(String(e)));",
    ),
    "/module.js": (
        "text/javascript",
        b"import {x} from './exports.js'; const {z} = await import('./dynamic.js');"
        b"const outcome = (run) => { try { run(); return 'ok'; } catch (e) { return String(e); } };"
        b"postMessage([x, z, import.meta.url.replace(location.origin, ''), outcome(() => importScripts('/lib.js'))]);",
    ),
    "/exports.js": ("text/javascript", b"export const x = 'imported';"),
    "/dynamic.js": ("text/javascript", b"export const z = 'imported dynamically';"),
    "/broken-graph.js": ("text/javascript", b"import './missing.js'; postMessage('ran');"),
    "/module-syntax.js": ("text/javascript", b"export const = 1;"),
    "/module-throws.js": ("text/javascript", b"throw new Error('in a module');"),
    "/classic-import.js": (
        "text/javascript",
        b"import('./dynamic.js').then((m) => postMessage(m.z), (e) => postMessage(String(e)));",
    ),
    "/imports-a-module.js": (
        "text/javascript",
        b"importScripts('/sub/runs-import.js');"
        b"self.run().then((m) => postMessage(m.where), (e) => postMessage(String(e)));",
    ),
    "/sub/runs-import.js": ("text/javascript", b"self.run = () => import('./where.js');"),
    # A module worker's root and what it imports statically, which goes with its creator's referrer policy.
    "/referrer-root.js": ("text/javascript", b"import './referrer-dep.js'; postMessage('ran');"),
    "/referrer-dep.js": ("text/javascript", b"export const x = 1;"),
    # Where /redirect-w.js ends up: a module worker's root that imports the URL it was asked for.
    "/v2/w.js": (
        "text/javascript",
        b"self.count = (self.count || 0) + 1;"
        b"import('/redirect-w.js').then(() => postMessage(self.count), (e) => postMessage(String(e)));",
    ),
    # The same script from another origin, whose response is opaque, so its base URL is about:blank.
    "/imports-a-cross-origin-module.js": (
        "text/javascript",
        b"importScripts(location.origin.replace('127.0.0.1', 'localhost') + '/sub/runs-import.js');"
        b"self.run().then((m) => postMessage(m.where), (e) => postMessage(String(e)));",
    ),
    "/sub/where.js": ("text/javascript", b"export const where = 'beside the imported script';"),
    "/port.js": (
        "text/javascript",
        b"onmessage = (e) => { const port = e.ports[0]; port.onmessage = (m) => port.postMessage(m.data + '!'); };",
    ),
    # A keepalive fetch that goes on after the worker that started it closes.
    "/keepalive.js": (
        "text/javascript",
        b"fetch('/beacon', { method: 'POST', body: 'kept', keepalive: true }); close();",
    ),
}

# The path and body of each POST that reached the server, in order.
_POSTED: list[tuple[str, bytes]] = []
# The Referer of each GET, by path, and the paths that redirect, to where.
_REFERERS: dict[str, str | None] = {}
_REDIRECTS = {"/redirect-w.js": "/v2/w.js"}


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
            _REFERERS[path] = self.headers.get("Referer")
            if path in _REDIRECTS:
                self.send_response(302)
                self.send_header("Location", _REDIRECTS[path])
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
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

        def do_POST(self) -> None:
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            _POSTED.append((self.path, body))
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

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
def test_a_workers_measure_from_a_timing_name_throws_as_in_chrome(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/measure.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    def timing(name: str) -> str:
        return (
            f"TypeError: Failed to execute 'measure' on 'Performance': When converting a mark name ('{name}') to a"
            " timestamp given a name that is a read only attribute in the PerformanceTiming interface, the global"
            " object has to be a Window object."
        )

    assert _evaluate(server, script) == [
        timing("navigationStart"),
        "SyntaxError: Failed to execute 'measure' on 'Performance': The mark 'nope' does not exist.",
        timing("loadEventEnd"),
        timing("navigationStart"),
    ]


@pytest.mark.usefixtures("page_program")
def test_a_worker_makes_marks_named_as_timing_attributes_as_in_chrome(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/timing-marks.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    # What Chrome 157.0.8103.79 gave, where a window's marks of these names throw.
    assert _evaluate(server, script) == ["navigationStart", "loadEventEnd", "number", "1"]


@pytest.mark.usefixtures("page_program")
def test_a_workers_high_entropy_values_are_the_windows(server: str) -> None:
    script = """Promise.all([
        new Promise((resolve) => {
            const worker = new Worker('/ua-data.js');
            worker.onmessage = (e) => resolve(e.data);
        }),
        navigator.userAgentData.getHighEntropyValues(['architecture', 'platformVersion', 'fullVersionList']),
    ])"""

    with mitto.Session() as session, session._open_page(f"{server}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        worker, window = page.evaluate(script, timeout=_TIMEOUT_SECONDS)
    assert worker == window
    assert worker["architecture"] != ""


@pytest.mark.usefixtures("page_program")
def test_a_workers_keepalive_fetch_outlives_the_worker(server: str) -> None:
    _POSTED.clear()
    script = """new Promise((resolve) => {
        new Worker('/keepalive.js');
        setTimeout(() => resolve('waited'), 500);
    })"""

    assert _evaluate(server, script) == "waited"
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while not _POSTED and time.monotonic() < deadline:
        time.sleep(0.05)
    assert _POSTED == [("/beacon", b"kept")]


@pytest.mark.usefixtures("page_program")
def test_a_worker_fetches_through_the_session(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/fetches.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    # The worker's own timeline has an entry of its request, as Chrome's does.
    assert _evaluate(server, script) == ["data from the server", True, [["/data.txt", "fetch"]]]


@pytest.mark.usefixtures("page_program")
def test_a_workers_stream_body_fails_over_http1_as_a_windows_does(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/uploads.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == "rejected TypeError: Failed to fetch / 1 / true"


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
def test_a_broadcast_reaches_the_window_from_a_worker_that_closes_at_once(server: str) -> None:
    script = """new Promise((resolve) => {
        const channel = new BroadcastChannel('news');
        channel.onmessage = (e) => resolve(e.data);
        new Worker('/broadcast-close.js');
        setTimeout(() => resolve('lost'), 5000);
    })"""

    assert _evaluate(server, script) == "last words"


@pytest.mark.usefixtures("page_program")
def test_a_document_with_busy_workers_gives_way_to_the_next(server: str) -> None:
    # The workers' requests and their ends, which the Session forgets with the document, go before the next document
    # or not at all, so the Page goes on.
    with mitto.Session() as session, session._open_page(f"{server}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        started = """new Promise((resolve) => {
            const worker = new Worker('/busy.js');
            worker.onmessage = (e) => resolve(e.data);
        })"""
        assert page.evaluate(started) == "busy"
        assert page.evaluate("location.href = '/?next'; 'set'") == "set"
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS).url == f"{server}/?next"
        assert page.evaluate(started) == "busy"
        assert page.evaluate("location.href = '/?last'; 'set'") == "set"
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS).url == f"{server}/?last"
        assert page.evaluate("location.search") == "?last"


@pytest.mark.usefixtures("page_program")
def test_a_workers_cache_questions_after_its_document_has_gone_go_nowhere(server: str) -> None:
    # The Session forgets the worker with its creator's document, so what the worker asks of the Cache API later goes
    # nowhere, and the Page goes on.
    with mitto.Session() as session, session._open_page(f"{server}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        started = """new Promise((resolve) => {
            const worker = new Worker('/busy-caches.js');
            worker.onmessage = (e) => resolve(e.data);
        })"""
        assert page.evaluate(started) == "busy"
        assert page.evaluate("location.href = '/?next'; 'set'") == "set"
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS).url == f"{server}/?next"
        # Past the worker's busy loop and what it asked then.
        assert page.evaluate("new Promise((resolve) => setTimeout(() => resolve(location.search), 2500))") == "?next"
        assert page.evaluate("caches.keys()") == []


@pytest.mark.usefixtures("page_program")
def test_a_workers_storage_calls_after_its_document_has_gone_go_nowhere(server: str) -> None:
    # What the forgotten worker asks of IndexedDB and Storage Buckets goes nowhere, and what waits for an answer goes
    # with its thread, so the Page goes on.
    with mitto.Session() as session, session._open_page(f"{server}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        started = """new Promise((resolve) => {
            const worker = new Worker('/busy-storage.js');
            worker.onmessage = (e) => resolve(e.data);
        })"""
        assert page.evaluate(started) == "busy"
        assert page.evaluate("location.href = '/?next'; 'set'") == "set"
        assert page.wait_for_navigation(timeout=_TIMEOUT_SECONDS).url == f"{server}/?next"
        assert page.evaluate("new Promise((resolve) => setTimeout(() => resolve(location.search), 2500))") == "?next"
        assert page.evaluate("indexedDB.databases().then((list) => list.map((info) => info.name))") == []


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
def test_a_worker_that_closes_in_its_locks_callback_lets_the_lock_go(server: str) -> None:
    script = """(async () => {
        const worker = new Worker('/lock-close.js');
        const got = await new Promise((resolve) => { worker.onmessage = (e) => resolve(e.data); });
        const granted = navigator.locks.request('x', () => 'granted');
        return [got, await Promise.race([granted, new Promise((resolve) => setTimeout(() => resolve('held'), 3000))])];
    })()"""

    assert _evaluate(server, script) == ["got", "granted"]


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
def test_a_worker_that_ends_lets_go_of_its_caches(server: str) -> None:
    # The cache and the unread body that the worker held go with it, so the cache, deleted after, gives its room back.
    script = """(async () => {
        const worker = new Worker('/hold-cache.js');
        const held = await new Promise((resolve) => { worker.onmessage = (e) => resolve(e.data); });
        const before = (await navigator.storage.estimate()).usage;
        worker.terminate();
        const deleted = await caches.delete('held');
        let after = before;
        for (const end = Date.now() + 5000; Date.now() < end && after >= before;) {
            await new Promise((resolve) => setTimeout(resolve, 100));
            after = (await navigator.storage.estimate()).usage;
        }
        return [held, deleted, before >= 1048576, before - after >= 1048576];
    })()"""

    assert _evaluate(server, script) == ["holding", True, True, True]


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
def test_a_worker_that_a_window_started_has_animation_frames(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/frames.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == [True, True, False]


@pytest.mark.usefixtures("page_program")
def test_a_workers_animation_frames_go_on_at_a_display_of_1000_hz(server: str) -> None:
    # Its period is shorter than a frame takes to arrive, and a thread that wakes late, as a loaded Host's does, misses
    # every deadline: a late frame is dropped, but never the one after it, so the frames go on.
    machine = mitto.Machine.draw(seed=4, refresh_rate=1000)
    script = """new Promise((resolve) => {
        const worker = new Worker('/frames.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""
    with mitto.Session(machine=machine) as session, session._open_page(f"{server}/", timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == [True, True, False]


@pytest.mark.usefixtures("page_program")
def test_a_worker_that_a_worker_started_has_no_animation_frames(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/nested-frames.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == (
        "NotSupportedError: Failed to execute 'requestAnimationFrame' on 'DedicatedWorkerGlobalScope': "
        "requestAnimationFrame not supported in this Worker."
    )


@pytest.mark.usefixtures("page_program")
def test_a_snapshot_of_user_activation_goes_only_from_a_window(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/activation.js');
        const answers = [];
        worker.onmessage = (e) => {
            answers.push(e.data);
            if (answers.length === 1) {
                worker.postMessage(null);
            } else {
                resolve(answers);
            }
        };
        worker.postMessage(null, {includeUserActivation: true});
    })"""

    # A Page has had no user activation, and a worker's port takes no snapshot, as in Chrome.
    assert _evaluate(server, script) == [[False, None], [None, None]]


@pytest.mark.usefixtures("page_program")
def test_a_worker_opens_the_storage_buckets_of_its_origin(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/buckets.js');
        worker.onmessage = async (e) => {
            const bucket = await navigator.storageBuckets.open('drafts');
            const response = await bucket.caches.match('/data.txt');
            resolve([e.data, await navigator.storageBuckets.keys(), response && (await response.text())]);
        };
    })"""

    assert _evaluate(server, script) == [["drafts", ["drafts"]], ["drafts"], "in the bucket"]


@pytest.mark.usefixtures("page_program")
def test_a_module_worker_runs_its_graph(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/module.js', {type: 'module'});
        worker.onerror = () => resolve('error');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == [
        "imported",
        "imported dynamically",
        "/module.js",
        "TypeError: Failed to execute 'importScripts' on 'WorkerGlobalScope': Module scripts don't support "
        "importScripts().",
    ]


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("path", ["/broken-graph.js", "/module-syntax.js", "/image.js"])
def test_a_module_worker_whose_graph_fails_fires_a_plain_error(server: str, path: str) -> None:
    script = f"""new Promise((resolve) => {{
        const worker = new Worker('{path}', {{type: 'module'}});
        worker.onmessage = (e) => resolve(e.data);
        worker.onerror = (e) => resolve([e.constructor.name, e.cancelable, 'message' in e]);
    }})"""

    assert _evaluate(server, script) == ["Event", True, False]


@pytest.mark.usefixtures("page_program")
def test_an_error_that_a_module_worker_throws_reaches_its_worker(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/module-throws.js', {type: 'module'});
        worker.onerror = (e) => {
            e.preventDefault();
            resolve([e.constructor.name, e.message, e.filename.replace(location.origin, ''), e.lineno]);
        };
    })"""

    assert _evaluate(server, script) == ["ErrorEvent", "Uncaught Error: in a module", "/module-throws.js", 1]


@pytest.mark.usefixtures("page_program")
def test_a_classic_worker_imports_a_module(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/classic-import.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == "imported dynamically"


@pytest.mark.usefixtures("page_program")
def test_import_in_an_imported_script_resolves_against_that_script(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/imports-a-module.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == "beside the imported script"


@pytest.mark.usefixtures("page_program")
def test_import_in_an_opaque_imported_script_has_no_base_url(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/imports-a-cross-origin-module.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == (
        "TypeError: Failed to resolve module specifier './where.js'. The base URL is about:blank because import() is "
        "called from a CORS-cross-origin script."
    )


@pytest.mark.usefixtures("page_program")
def test_a_module_workers_static_imports_take_its_creators_referrer_policy(server: str) -> None:
    # Chrome fetches a module worker's top-level graph through its creator's settings.
    _REFERERS.clear()
    script = """new Promise((resolve) => {
        const meta = document.createElement('meta');
        meta.name = 'referrer';
        meta.content = 'origin';
        document.head.append(meta);
        const worker = new Worker('/referrer-root.js', {type: 'module'});
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == "ran"
    assert _REFERERS["/referrer-dep.js"] == f"{server}/"


@pytest.mark.usefixtures("page_program")
def test_a_module_workers_root_is_in_its_map_at_the_url_it_was_asked_for(server: str) -> None:
    # After a redirect, an import() of the URL that new Worker() asked for finds the root, which runs once.
    script = """new Promise((resolve) => {
        const worker = new Worker('/redirect-w.js', {type: 'module'});
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == 1


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
