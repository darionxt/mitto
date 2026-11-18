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
    "/closes.js": ("text/javascript", b"postMessage('before'); close(); postMessage('after'); setTimeout(() => postMessage('timer'));"),
    "/html.js": ("text/html", b"postMessage('ran as text/html');"),
    "/image.js": ("image/png", b"postMessage('ran as an image');"),
    "/nested.js": (
        "text/javascript",
        b"const inner = new Worker('/echo.js', {name: 'inner'});"
        b"inner.onmessage = (e) => postMessage(['from inner', e.data]); inner.postMessage('ping');",
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
            if self.path in _SCRIPTS:
                content_type, body = _SCRIPTS[self.path]
                status = 200
            elif self.path == "/":
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
