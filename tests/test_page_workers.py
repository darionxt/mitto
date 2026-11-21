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
def test_a_worker_fetches_through_the_session(server: str) -> None:
    script = """new Promise((resolve) => {
        const worker = new Worker('/fetches.js');
        worker.onmessage = (e) => resolve(e.data);
    })"""

    assert _evaluate(server, script) == ["data from the server", True]


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
def test_a_broadcast_goes_between_a_window_and_its_worker(server: str) -> None:
    script = """new Promise((resolve) => {
        const channel = new BroadcastChannel('news');
        channel.onmessage = (e) => resolve([e.data, e.origin === location.origin]);
        const worker = new Worker('/broadcast.js');
        worker.onmessage = () => channel.postMessage('from the window');
    })"""

    assert _evaluate(server, script) == [["worker heard", "from the window", server], True]
