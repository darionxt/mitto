"""Tests of what a Page has from Chrome's renderer beyond the Web IDL files: window.chrome, console.memory and
performance.memory, and V8 as Chrome's renderer starts and sets it up.

The expected values are what real Chrome 157.0.8103.39 gave on a new page at an HTTP URL of the loopback address, on
Windows, with and without its variations seed (#216): the shapes of the objects, their order and Chrome's messages,
and of the values only what says nothing of the machine. Most tests run the Page program that MITTO_PAGE_PROGRAM names,
as tests/test_page_document.py does. Those that need a document at an HTTP URL, or a Session's Machine, open a Page
through a Session, and need a native library with the Page program beside it.
"""

import dataclasses
import http.server
import json
import os
import subprocess
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

# What a function that Chrome's renderer makes with v8::Function::New() and no name looks like.
_ANONYMOUS_FUNCTION = {
    "name": "",
    "length": 0,
    "source": "function () { [native code] }",
    "own": ["length", "name", "prototype"],
    "constructor": True,
}

# Blink's QuantizeMemorySize() buckets, worked out in single precision as Blink works them out, and a helper that
# describes a property for the tests below.
_HELPERS = """
const buckets = (() => {
  const list = [];
  let next = Math.fround(10000000);
  const scaling = Math.fround(Math.exp(Math.log(Math.fround(4000000000) / next) / 100));
  let power = 100000000;
  let granularity = power / 1000;
  for (let i = 0; i < 100; i++) {
    const current = Math.floor(next);
    list.push(current - (current % granularity));
    next = Math.fround(next * scaling);
    if (next >= power) { power *= 10; granularity *= 10; }
  }
  return list;
})();
const isConstructor = (f) => { try { Reflect.construct(String, [], f); return true; } catch { return false; } };
const fn = (f) => ({name: f.name, length: f.length, source: Function.prototype.toString.call(f),
  own: Reflect.ownKeys(f).map(String), constructor: isConstructor(f)});
const attributes = (o, k) => {
  const d = Object.getOwnPropertyDescriptor(o, k);
  return ["writable", "enumerable", "configurable", "get", "set"].filter((a) => d[a] === true || typeof d[a] === "function");
};
const error = (f) => { try { f(); return null; } catch (e) { return `${e.constructor.name}: ${e.message}`; } };
"""


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, script: str, *options: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *options, "-"],
        input=script,
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )


def _evaluate(program: Path, expression: str, *options: str) -> Any:
    """Return what `expression`, after the helpers, gives as JSON, or what its promise was fulfilled with."""
    result = _run(
        program, f"{_HELPERS}\nPromise.resolve({expression}).then((value) => JSON.stringify(value))", *options
    )
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def test_window_chrome_has_what_chrome_puts_on_a_page(program: Path) -> None:
    shape = _evaluate(
        program,
        """({
          global: attributes(window, "chrome"),
          proto: Object.getPrototypeOf(chrome) === Object.prototype,
          keys: Reflect.ownKeys(chrome),
          attributes: Reflect.ownKeys(chrome).map((k) => attributes(chrome, k)),
          loadTimes: fn(chrome.loadTimes),
          csi: fn(chrome.csi),
          prototype: Reflect.ownKeys(chrome.loadTimes.prototype),
        })""",
    )

    assert shape == {
        "global": ["writable", "enumerable", "configurable"],
        "proto": True,
        "keys": ["loadTimes", "csi", "app"],
        "attributes": [["writable", "enumerable", "configurable"]] * 3,
        "loadTimes": _ANONYMOUS_FUNCTION,
        "csi": _ANONYMOUS_FUNCTION,
        "prototype": ["constructor"],
    }


def test_window_chrome_comes_before_what_the_context_decides(program: Path) -> None:
    # Chrome adds it once the global object has what Window's template gives it, V8's builtins last, and before its
    # conditional features, so in Chrome it comes between WebAssembly and crashReport.
    around = _evaluate(
        program,
        """[Object.getOwnPropertyNames(window), Object.keys(window)].map(
             (n) => n.slice(n.indexOf("chrome") - 2, n.indexOf("chrome") + 3))""",
    )

    assert around == [
        ["Temporal", "WebAssembly", "chrome", "crashReport", "cookieStore"],
        ["webkitCancelAnimationFrame", "webkitRequestAnimationFrame", "chrome", "crashReport", "cookieStore"],
    ]


def test_load_times_and_csi_give_chrome_s_members(program: Path) -> None:
    result = _evaluate(
        program,
        """(() => {
          const loadTimes = chrome.loadTimes();
          const csi = chrome.csi();
          const types = (o) => Object.fromEntries(Reflect.ownKeys(o).map((k) => [k, typeof o[k]]));
          const before = loadTimes.requestTime;
          loadTimes.requestTime = 5;
          return {
            loadTimes: types(loadTimes),
            csi: types(csi),
            attributes: attributes(loadTimes, "requestTime"),
            kept: loadTimes.requestTime === before,
            deleted: delete loadTimes.startLoadTime && !("startLoadTime" in loadTimes),
            fresh: chrome.loadTimes() !== chrome.loadTimes(),
            constructed: Reflect.ownKeys(new chrome.csi()),
            values: [loadTimes.navigationType, loadTimes.firstPaintAfterLoadTime, loadTimes.wasAlternateProtocolAvailable,
                     csi.tran],
            seconds: loadTimes.requestTime * 1000 === csi.startE,
          };
        })()""",
    )

    assert result == {
        "loadTimes": {
            "requestTime": "number",
            "startLoadTime": "number",
            "commitLoadTime": "number",
            "finishDocumentLoadTime": "number",
            "finishLoadTime": "number",
            "firstPaintTime": "number",
            "firstPaintAfterLoadTime": "number",
            "navigationType": "string",
            "wasFetchedViaSpdy": "boolean",
            "wasNpnNegotiated": "boolean",
            "npnNegotiatedProtocol": "string",
            "wasAlternateProtocolAvailable": "boolean",
            "connectionInfo": "string",
        },
        "csi": {"startE": "number", "onloadT": "number", "pageT": "number", "tran": "number"},
        # Native data properties, with a setter that ignores what a page assigns.
        "attributes": ["writable", "enumerable", "configurable"],
        "kept": True,
        "deleted": True,
        "fresh": True,
        "constructed": ["startE", "onloadT", "pageT", "tran"],
        "values": ["Other", 0, False, 15],
        "seconds": True,
    }


def test_chrome_app_is_what_the_extensions_system_gives_a_web_page(program: Path) -> None:
    result = _evaluate(
        program,
        """(async () => {
          const app = chrome.app;
          const methods = ["getDetails", "getIsInstalled", "installState", "runningState"];
          const order = [];
          const state = new Promise((resolve) => {
            const returned = app.installState((s) => { order.push("callback"); resolve(s); });
            order.push(returned === undefined ? "returned" : "returned a value");
          });
          app.isInstalled = true;
          return {
            keys: Reflect.ownKeys(app),
            attributes: Reflect.ownKeys(app).map((k) => attributes(app, k)),
            methods: methods.map((m) => fn(app[m])),
            answers: [app.isInstalled, app.getIsInstalled(), app.getDetails(), app.runningState(),
                      app.runningState.call(null), app.installState(), app.installState(null)],
            errors: [error(() => app.getDetails(1)), error(() => app.getIsInstalled(undefined)),
                     error(() => app.installState(1)), error(() => app.installState(() => {}, 1)),
                     error(() => new app.getDetails())],
            enums: [Object.entries(app.InstallState), Object.entries(app.RunningState)],
            state: await state,
            order,
          };
        })()""",
    )

    assert result == {
        "keys": [
            "isInstalled",
            "getDetails",
            "getIsInstalled",
            "installState",
            "runningState",
            "InstallState",
            "RunningState",
        ],
        "attributes": [["writable", "enumerable", "configurable"]] * 7,
        "methods": [
            {
                "name": name,
                "length": 0,
                "source": f"function {name}() {{ [native code] }}",
                "own": ["length", "name"],
                "constructor": False,
            }
            for name in ["getDetails", "getIsInstalled", "installState", "runningState"]
        ],
        "answers": [False, False, None, "cannot_run", "cannot_run", None, None],
        "errors": [
            "TypeError: Error in invocation of app.getDetails(): ",
            "TypeError: Error in invocation of app.getIsInstalled(): ",
            "TypeError: Error in invocation of app.installState(optional function callback): ",
            "TypeError: Error in invocation of app.installState(optional function callback): ",
            "TypeError: app.getDetails is not a constructor",
        ],
        "enums": [
            [["DISABLED", "disabled"], ["INSTALLED", "installed"], ["NOT_INSTALLED", "not_installed"]],
            [["CANNOT_RUN", "cannot_run"], ["READY_TO_RUN", "ready_to_run"], ["RUNNING", "running"]],
        ],
        "state": "not_installed",
        "order": ["returned", "callback"],
    }


def test_console_has_what_v8_s_inspector_gives_a_window(program: Path) -> None:
    result = _evaluate(
        program,
        """(() => {
          const descriptor = Object.getOwnPropertyDescriptor(console, "memory");
          const memory = console.memory;
          const prototype = Object.getPrototypeOf(memory);
          console.memory = 1;
          const task = console.createTask("x");
          return {
            last: Reflect.ownKeys(console).slice(-4).map(String),
            accessor: [attributes(console, "memory"), fn(descriptor.get), fn(descriptor.set)],
            memory: [Object.prototype.toString.call(memory), Reflect.ownKeys(memory), console.memory === memory,
                     Object.getPrototypeOf(prototype) === Object.prototype, "MemoryInfo" in window],
            prototype: Reflect.ownKeys(prototype).map(String),
            getter: fn(Object.getOwnPropertyDescriptor(prototype, "jsHeapSizeLimit").get),
            other: error(() => Object.getOwnPropertyDescriptor(prototype, "usedJSHeapSize").get.call({})),
            kept: Object.prototype.toString.call(console.memory),
            createTask: [fn(console.createTask), Reflect.ownKeys(task), task.run(() => 5),
                         error(() => console.createTask("")), error(() => task.run(1))],
          };
        })()""",
    )

    assert result == {
        "last": ["context", "createTask", "memory", "Symbol(Symbol.toStringTag)"],
        "accessor": [
            ["enumerable", "configurable", "get", "set"],
            {
                "name": "",
                "length": 0,
                "source": "function () { [native code] }",
                "own": ["length", "name"],
                "constructor": False,
            },
            {
                "name": "",
                "length": 1,
                "source": "function () { [native code] }",
                "own": ["length", "name"],
                "constructor": False,
            },
        ],
        "memory": ["[object MemoryInfo]", [], False, True, False],
        "prototype": ["totalJSHeapSize", "usedJSHeapSize", "jsHeapSizeLimit", "Symbol(Symbol.toStringTag)"],
        "getter": {
            "name": "get jsHeapSizeLimit",
            "length": 0,
            "source": "function get jsHeapSizeLimit() { [native code] }",
            "own": ["length", "name"],
            "constructor": False,
        },
        "other": "TypeError: Illegal invocation",
        "kept": "[object MemoryInfo]",
        "createTask": [
            {
                "name": "createTask",
                "length": 0,
                "source": "function createTask() { [native code] }",
                "own": ["length", "name"],
                "constructor": False,
            },
            ["run"],
            5,
            "Error: First argument must be a non-empty string.",
            "Error: First argument must be a function.",
        ],
    }


def test_memory_is_bucketized_where_the_process_is_not_locked_to_a_site(program: Path) -> None:
    # The command line's about:blank is at no site, so performance.memory is bucketized too, from the one cache.
    in_buckets = _evaluate(
        program,
        """[console.memory, performance.memory].map((m) =>
             [m.usedJSHeapSize, m.totalJSHeapSize, m.jsHeapSizeLimit].every((size) => buckets.includes(size)))""",
    )

    assert in_buckets == [True, True]


def test_v8_is_set_up_as_on_chrome_s_main_thread(program: Path) -> None:
    result = _evaluate(
        program,
        """(() => {
          const shared = new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer;
          const samples = Array.from({length: 50}, () => Temporal.Now.instant().epochNanoseconds);
          return {
            sharedArrayBuffer: [typeof SharedArrayBuffer, Object.prototype.toString.call(shared)],
            wait: error(() => Atomics.wait(new Int32Array(shared), 0, 0, 0)),
            waitAsync: Atomics.waitAsync(new Int32Array(shared), 0, 0, 0).value,
            atomics: Reflect.ownKeys(Atomics).map(String),
            webAssembly: Reflect.ownKeys(WebAssembly).map(String),
            streaming: [fn(WebAssembly.compileStreaming), fn(WebAssembly.instantiateStreaming)],
            module: error(() => new WebAssembly.Module(new Uint8Array(8 * 1024 * 1024 + 1))),
            atTheLimit: error(() => new WebAssembly.Module(new ArrayBuffer(8 * 1024 * 1024))),
            isError: [Error.isError(new DOMException("x")), Error.isError(Object.create(DOMException.prototype))],
            temporal: samples.every((ns) => ns % 100000n === 0n),
          };
        })()""",
    )

    assert result == {
        "sharedArrayBuffer": ["undefined", "[object SharedArrayBuffer]"],
        "wait": "TypeError: Atomics.wait cannot be called in this context",
        "waitAsync": "timed-out",
        "atomics": [
            "load",
            "store",
            "add",
            "sub",
            "and",
            "or",
            "xor",
            "exchange",
            "compareExchange",
            "isLockFree",
            "wait",
            "waitAsync",
            "notify",
            "pause",
            "Symbol(Symbol.toStringTag)",
        ],
        "webAssembly": [
            "compile",
            "validate",
            "instantiate",
            "Module",
            "Instance",
            "Table",
            "Memory",
            "Global",
            "Tag",
            "JSTag",
            "Exception",
            "CompileError",
            "LinkError",
            "RuntimeError",
            "compileStreaming",
            "instantiateStreaming",
            "Suspending",
            "promising",
            "SuspendError",
            "Symbol(Symbol.toStringTag)",
        ],
        "streaming": [
            {
                "name": name,
                "length": 1,
                "source": f"function {name}() {{ [native code] }}",
                "own": ["length", "name"],
                "constructor": False,
            }
            for name in ["compileStreaming", "instantiateStreaming"]
        ],
        "module": "RangeError: WebAssembly.Compile is disallowed on the main thread, if the buffer size is larger than "
        "8MB. Use WebAssembly.compile, compile on a worker thread, or use the flag "
        "`--enable-features=WebAssemblyUnlimitedSyncCompilation`.",
        "atTheLimit": "CompileError: WebAssembly.Module(): expected magic word 00 61 73 6d, found 00 00 00 00 @+0",
        "isError": [True, False],
        "temporal": True,
    }


def test_js_flags_come_after_the_engine_release_s(program: Path) -> None:
    # The Engine Release's flags keep SharedArrayBuffer for cross-origin isolated contexts. One of --js-flags= that
    # turns that off again wins, as it does in Chrome.
    assert _evaluate(program, "typeof SharedArrayBuffer", "--js-flags=--no-enable-sharedarraybuffer-per-context") == (
        "function"
    )
    assert _evaluate(program, "typeof SharedArrayBuffer", "--js-flags= --no-turbolev , --maglev") == "undefined"


_EMPTY_MODULE = "new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])"


@pytest.mark.parametrize(
    ("argument", "outcome"),
    [
        (
            f"new Response({_EMPTY_MODULE}, {{headers: {{'content-type': 'application/wasm'}}}})",
            "[object WebAssembly.Module]",
        ),
        (
            f"Promise.resolve(new Response({_EMPTY_MODULE}, {{headers: {{'content-type': 'Application/WASM'}}}}))",
            "[object WebAssembly.Module]",
        ),
        (
            "undefined",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': An argument must be provided, which "
            "must be a Response or Promise<Response> object",
        ),
        (
            "Promise.resolve(1)",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': An argument must be provided, "
            "which must be a Response or Promise<Response> object",
        ),
        ("Promise.reject(new Error('r'))", "Error: r"),
        (
            f"new Response({_EMPTY_MODULE})",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': Incorrect response MIME type. Expected "
            "'application/wasm'.",
        ),
        (
            f"new Response({_EMPTY_MODULE}, {{headers: {{'content-type': 'application/wasm; charset=utf-8'}}}})",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': Incorrect response MIME type. Expected "
            "'application/wasm'.",
        ),
        (
            f"new Response({_EMPTY_MODULE}, {{status: 404, headers: {{'content-type': 'application/wasm'}}}})",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': HTTP status code is not ok",
        ),
        (
            f"(async () => {{ const r = new Response({_EMPTY_MODULE}, {{headers: {{'content-type': 'application/wasm'}}}}); "
            "await r.arrayBuffer(); return r; })()",
            "TypeError: Failed to execute 'compile' on 'WebAssembly': Cannot compile WebAssembly.Module from an already "
            "read Response",
        ),
        (
            "new Response(null, {headers: {'content-type': 'application/wasm'}})",
            "CompileError: Failed to execute 'compile' on 'WebAssembly': Empty WebAssembly module",
        ),
        (
            "new Response(new Uint8Array([1, 2, 3]), {headers: {'content-type': 'application/wasm'}})",
            "CompileError: WebAssembly.compileStreaming(): expected 4 bytes, fell off end @+0",
        ),
    ],
)
def test_compile_streaming_reads_a_response_as_blink_does(program: Path, argument: str, outcome: str) -> None:
    script = (
        f"WebAssembly.compileStreaming({argument}).then((m) => Object.prototype.toString.call(m), "
        "(e) => `${e.constructor.name}: ${e.message}`)"
    )

    assert _evaluate(program, script) == outcome


def test_instantiate_streaming_compiles_and_instantiates(program: Path) -> None:
    script = (
        f"WebAssembly.instantiateStreaming(new Response({_EMPTY_MODULE}, {{headers: {{'content-type': "
        "'application/wasm'}})).then((r) => [Reflect.ownKeys(r), Object.prototype.toString.call(r.instance)])"
    )

    assert _evaluate(program, script) == [["module", "instance"], "[object WebAssembly.Instance]"]


class _PageServer:
    """Serves one page and an empty WebAssembly module on the loopback address."""

    def __init__(self) -> None:
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                if self.path == "/empty.wasm":
                    body, content_type = bytes([0, 97, 115, 109, 1, 0, 0, 0]), "application/wasm"
                else:
                    body, content_type = b"<!doctype html><title>Page</title><p>Hello", "text/html; charset=utf-8"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def __enter__(self) -> "_PageServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_PageServer]:
    with _PageServer() as page_server:
        yield page_server


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _heap_size_limit(memory_gib: int) -> int:
    """Return V8's heap_size_limit for a 64-bit desktop with `memory_gib` of memory, as ConfigureDefaults() works it
    out (v8/src/heap/heap.cc): half the memory for the old generation, at most 4 GiB, and three semi-spaces of a
    128th of the memory, from 2 to 32 MiB, each rounded up to V8's page of 256 KiB. A desktop of at most 2048 MiB is
    low-end, and Chrome's renderer gives it --optimize-for-size, which holds a semi-space to 1 MiB."""
    mib, page = 1 << 20, 256 << 10
    memory = memory_gib << 30
    old_generation = -(-min(memory // 2, 4 << 30) // page) * page
    semi_space = -(-min(max(memory // 4 // 32, 2 * mib), 32 * mib) // page) * page
    if memory <= 2048 * mib:
        semi_space = mib
    return old_generation + 3 * semi_space


@pytest.mark.usefixtures("page_program")
def test_a_document_s_load_times_follow_its_loading(server: _PageServer) -> None:
    with mitto.Session() as session, _open_page(session, server.url()) as page:
        result = page.evaluate(
            """(() => {
              const t = chrome.loadTimes();
              const csi = chrome.csi();
              return {
                order: t.requestTime === t.startLoadTime && t.requestTime <= t.commitLoadTime &&
                       t.commitLoadTime <= t.finishDocumentLoadTime && t.finishDocumentLoadTime <= t.finishLoadTime,
                origin: performance.timeOrigin - t.requestTime * 1000,
                milliseconds: [t.requestTime, t.commitLoadTime, t.finishLoadTime].every(
                  (s) => Math.abs(Math.round(s * 1000) - s * 1000) < 1e-3),
                csi: [csi.startE === t.requestTime * 1000, csi.onloadT === t.finishDocumentLoadTime * 1000,
                      csi.pageT > 0, csi.tran],
                paint: t.firstPaintTime,
                connection: [t.wasFetchedViaSpdy, t.wasNpnNegotiated, t.npnNegotiatedProtocol, t.connectionInfo],
              };
            })()"""
        )

    # As in Chrome, requestTime is the navigation's start coarsened to 100 microseconds and cut to whole milliseconds,
    # and timeOrigin the same start coarsened by the same clamper, so it is up to 0.9 ms later. Each takes the wall
    # clock at its own moment, as DocumentLoadTiming and Performance do, so the two can land a step of 0.1 ms apart, and
    # their doubles lose about 0.0002 ms at this size.
    origin = result.pop("origin")
    assert -0.1 - 1e-3 <= origin <= 1.0 + 1e-3
    assert result == {
        "order": True,
        "milliseconds": True,
        "csi": [True, True, True, 15],
        # The Runtime paints nothing.
        "paint": 0,
        # Chrome's for a page over HTTP/1.1 with no TLS.
        "connection": [False, False, "unknown", "http/1.1"],
    }


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("memory", [2, 4, 8, 16])
def test_performance_memory_is_precise_at_a_site_and_follows_the_machine(server: _PageServer, memory: int) -> None:
    machine = dataclasses.replace(mitto.Machine.draw(seed=216), memory=memory)
    with mitto.Session(machine=machine) as session, _open_page(session, server.url()) as page:
        limit = page.evaluate("performance.memory.jsHeapSizeLimit")
        compiled = page.evaluate(
            "WebAssembly.compileStreaming(fetch('/empty.wasm')).then((m) => Object.prototype.toString.call(m))"
        )

    assert limit == _heap_size_limit(memory)
    assert compiled == "[object WebAssembly.Module]"
