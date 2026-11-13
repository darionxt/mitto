"""Tests of Web Locks in a Page that a Session opened, on a local server.

They need an origin that is not opaque, and so a Page that a Session opened, with a native library and the Page program
beside it. The expected values are what Chrome 157 gave for the same scripts on the same kind of page, headless on macOS
(157.0.8103.51), once the page had loaded.
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
_BLANK = b"<!doctype html><title>c</title><body>"

_LOCKS = """
new Promise(async (resolve) => {
  const out = {};
  const error = async (f) => { try { await f(); return 'none'; } catch (e) { return `${e.name}: ${e.message}`; } };
  out.errors = {
    dash: await error(() => navigator.locks.request('-a', () => {})),
    stealIfAvailable: await error(() => navigator.locks.request('a', {steal: true, ifAvailable: true}, () => {})),
    stealShared: await error(() => navigator.locks.request('a', {steal: true, mode: 'shared'}, () => {})),
    signalIfAvailable: await error(() => navigator.locks.request('a', {signal: new AbortController().signal, ifAvailable: true}, () => {})),
    signalSteal: await error(() => navigator.locks.request('a', {signal: new AbortController().signal, steal: true}, () => {})),
    noCallback: await error(() => navigator.locks.request('a')),
    badMode: await error(() => navigator.locks.request('a', {mode: 'x'}, () => {})),
    aborted: await error(() => navigator.locks.request('a', {signal: AbortSignal.abort('why')}, () => {})),
    construct: await error(() => new Lock()),
  };
  const log = [];
  const first = navigator.locks.request('order', (lock) => { log.push('granted'); return 'v'; });
  setTimeout(() => log.push('timeout0'), 0);
  Promise.resolve().then(() => log.push('micro'));
  out.firstValue = await first;
  out.order = log;
  const log2 = [];
  const second = navigator.locks.request('order2', () => { log2.push('granted'); });
  setTimeout(() => log2.push('timeout0'), 0);
  await second;
  await new Promise(r => setTimeout(r, 10));
  out.order2 = log2;
  let release;
  const held = navigator.locks.request('x', (lock) => {
    out.lock = [Object.prototype.toString.call(lock), lock.name, lock.mode, Object.getOwnPropertyNames(lock)];
    return new Promise(r => { release = r; });
  });
  const shared1 = navigator.locks.request('s', {mode: 'shared'}, () => new Promise(() => {}));
  const shared2 = navigator.locks.request('s', {mode: 'shared'}, () => new Promise(() => {}));
  const waiting = navigator.locks.request('x', {mode: 'shared'}, (lock) => 'second');
  const ifAvailable = await navigator.locks.request('x', {ifAvailable: true}, (lock) => lock);
  out.ifAvailable = ifAvailable;
  await new Promise(r => setTimeout(r, 10));
  const snapshot = await navigator.locks.query();
  out.snapshot = {
    keys: Object.keys(snapshot),
    held: snapshot.held.map(l => [l.name, l.mode, Object.keys(l)]),
    pending: snapshot.pending.map(l => [l.name, l.mode]),
    clientId: snapshot.held[0].clientId.replace(/[0-9A-F]/g, 'X').replace(/[0-9a-f]/g, 'x'),
    sameClient: new Set(snapshot.held.concat(snapshot.pending).map(l => l.clientId)).size,
  };
  release('done');
  out.held = await held;
  out.waiting = await waiting;
  const controller = new AbortController();
  let stuck;
  const blocker = navigator.locks.request('y', () => new Promise(r => { stuck = r; }));
  const aborted = navigator.locks.request('y', {signal: controller.signal}, () => 'never');
  controller.abort();
  out.abortedWhileWaiting = await aborted.then(v => v, e => `${e.name}: ${e.message}`);
  const stolen = navigator.locks.request('y', {steal: true}, () => 'stole');
  out.blocker = await blocker.then(v => v, e => `${e.name}: ${e.message}`);
  out.stolen = await stolen;
  out.thrown = await navigator.locks.request('z', () => { throw new TypeError('in callback'); }).then(v => v, e => `${e.name}: ${e.message}`);
  resolve(out);
})
"""

_LOCKS_EXPECTED: dict[str, Any] = {
    "errors": {
        "dash": "NotSupportedError: Failed to execute 'request' on 'LockManager': Names cannot start with '-'.",
        "stealIfAvailable": "NotSupportedError: Failed to execute 'request' on 'LockManager': The "
        "'steal' and 'ifAvailable' options cannot be used together.",
        "stealShared": "NotSupportedError: Failed to execute 'request' on 'LockManager': The 'steal' "
        "option may only be used with 'exclusive' locks.",
        "signalIfAvailable": "NotSupportedError: Failed to execute 'request' on 'LockManager': The "
        "'signal' and 'ifAvailable' options cannot be used together.",
        "signalSteal": "NotSupportedError: Failed to execute 'request' on 'LockManager': The 'signal' "
        "and 'steal' options cannot be used together.",
        "noCallback": "TypeError: Failed to execute 'request' on 'LockManager': 2 arguments required, "
        "but only 1 present.",
        "badMode": "TypeError: Failed to execute 'request' on 'LockManager': Failed to read the 'mode' "
        "property from 'LockOptions': The provided value 'x' is not a valid enum value of "
        "type LockMode.",
        "aborted": "undefined: undefined",
        "construct": "TypeError: Failed to construct 'Lock': Illegal constructor",
    },
    "firstValue": "v",
    "order": ["micro", "timeout0", "granted"],
    "order2": ["timeout0", "granted"],
    "lock": ["[object Lock]", "x", "exclusive", []],
    "ifAvailable": None,
    "snapshot": {
        "keys": ["held", "pending"],
        "held": [
            ["s", "shared", ["clientId", "mode", "name"]],
            ["s", "shared", ["clientId", "mode", "name"]],
            ["x", "exclusive", ["clientId", "mode", "name"]],
        ],
        "pending": [["x", "shared"]],
        "clientId": "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
        "sameClient": 1,
    },
    "held": "done",
    "waiting": "second",
    "abortedWhileWaiting": "AbortError: signal is aborted without reason",
    "blocker": "AbortError: Lock broken by another request with the 'steal' option.",
    "stolen": "stole",
    "thrown": "TypeError: in callback",
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def blank_page() -> Iterator[str]:
    """The URL of a blank page on 127.0.0.1, which is a secure context."""

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
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


def _evaluate(url: str, script: str) -> Any:
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        return page.evaluate(f"Promise.resolve(({script}))")


@pytest.mark.usefixtures("page_program")
def test_web_locks_queue_steal_and_abort_as_chrome_s(blank_page: str) -> None:
    # A grant lands after a timer set beside its request, as Chrome's comes over Mojo.
    assert _evaluate(blank_page, _LOCKS) == _LOCKS_EXPECTED


@pytest.mark.usefixtures("page_program")
def test_a_waiting_request_keeps_a_signal_that_only_it_holds(blank_page: str) -> None:
    # The signal of AbortSignal.any() is held only by the request, which keeps it while it waits, as Blink's abort
    # handle does, so that collections in between do not lose its abort. Chrome 157 rejects the request.
    script = """
      (async () => {
        let release;
        await new Promise((granted) => navigator.locks.request('held', () => {
          granted();
          return new Promise((r) => { release = r; });
        }));
        const controller = new AbortController();
        const waiting = navigator.locks.request('held', {signal: AbortSignal.any([controller.signal])}, () => 'ran')
            .then((v) => 'fulfilled ' + v, (e) => 'rejected ' + e.name);
        // Enough garbage for the collector to run a few times.
        for (let i = 0; i < 40; i++) {
          const a = [];
          for (let j = 0; j < 200000; j++) a.push({j, s: 'x' + j});
        }
        await new Promise((r) => setTimeout(r, 50));
        controller.abort();
        const result = await Promise.race([waiting, new Promise((r) => setTimeout(() => r('still pending'), 500))]);
        release();
        return result;
      })()
    """

    assert _evaluate(blank_page, script) == "rejected AbortError"
