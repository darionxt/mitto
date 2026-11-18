"""Tests of how a Page runs module scripts: their order beside classic scripts, their graphs and errors, import(),
import.meta, top-level await, import maps, and JSON and text modules.

Each case is a page that logs what runs, in order, and the local server of `tests.script_pages`, which holds a response
until the page sends a mark, so that an order does not hang on the network's timing. The orders are what Chrome
157.0.8103.51 and Chrome for Testing 157.0.8103.79 gave, headless on macOS against the same pages and server. Each test goes through both APIs, and needs a
native library with the Page program beside it, as CI's native job installs it on every Host.
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.script_pages import Case, Route, ScriptServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_JAVASCRIPT = "text/javascript"
_JSON = "application/json"
_HTML = "text/html; charset=utf-8"

# What every case's page starts with: `log()` and `finish()`, which settles `done` with what was logged, a mark that
# releases what the server holds, `state()`, the ready state and the current script, and `path()`, a URL without the
# origin.
_PRELUDE = (
    "<!doctype html><script>"
    "window.logged = [];"
    "function log(x) { logged.push(String(x)); }"
    "window.done = new Promise(resolve => { window.finish = resolve; });"
    "done.then(l => fetch('/result', {method: 'POST', body: JSON.stringify(l)}));"
    "setTimeout(() => { log('gave up'); finish(logged); }, 5000);"
    "function mark(name) { fetch('/mark/' + name); }"
    "function current() { const s = document.currentScript; return s === null ? 'null' : s.id || 'anonymous'; }"
    "function state() { return document.readyState + ' ' + current(); }"
    "function path(url) { return String(url).replace(location.origin, ''); }"
    "addEventListener('error', e => log('window error ' + e.message + ' at ' + path(e.filename) + ':' + e.lineno));"
    "</script>"
)

# A listener that ends the case a moment after the window's load event.
_FINISH_AFTER_LOAD = (
    "<script>addEventListener('load', () => { log('load ' + state()); setTimeout(() => finish(logged), 200); });"
    "</script>"
)


def _page(markup: str) -> Route:
    return Route(_PRELUDE + markup, _HTML)


def _js(source: str, hold: str | None = None, status: int = 200, content_type: str = _JAVASCRIPT) -> Route:
    return Route(source, content_type, status, hold=hold)


_CASES: dict[str, Case] = {}

# Module scripts are deferred, in document order with the deferred classic ones, and run before DOMContentLoaded with
# no current script; an async one runs once it is ready, and holds the load event back until it has run.
_CASES["order"] = Case(
    {
        "/": _page(
            _FINISH_AFTER_LOAD + "<script>"
            "document.addEventListener('DOMContentLoaded', () => { log('DOMContentLoaded ' + state()); mark('dcl'); });"
            "</script>"
            "<script type=module id=m1 src=/m1.js></script>"
            "<script defer id=d1 src=/d1.js></script>"
            "<script type=module id=i1>log('i1 ' + state()); log('i1 meta ' + path(import.meta.url));</script>"
            "<script type=module async id=a1 src=/a1.js></script>"
            "<script id=c1>log('c1 ' + state())</script>"
            "<p>end</p>"
        ),
        "/m1.js": _js("import './dep.js'; log('m1 ' + state()); log('m1 meta ' + path(import.meta.url));"),
        "/dep.js": _js("log('dep ' + state());"),
        "/d1.js": _js("log('d1 ' + state());"),
        "/a1.js": _js("log('a1 ' + state());", hold="dcl"),
    }
)

# A graph: each module once, after what it imports, in the order of its imports, a cycle included; load at an external
# module's element once its graph has run, and none at an inline one's.
_CASES["graph"] = Case(
    {
        "/": _page(
            _FINISH_AFTER_LOAD + "<script type=module id=root src=/root.js onload=\"log('root load')\"></script>"
            "<script type=module id=inline onload=\"log('inline load')\">"
            "import {value} from '/b.js'; log('inline ' + value);</script>"
        ),
        "/root.js": _js("import '/a.js'; import {value} from '/b.js'; log('root ' + value);"),
        "/a.js": _js("import '/c.js'; log('a');"),
        "/b.js": _js("import '/c.js'; export const value = 'b'; log('b');"),
        "/c.js": _js("import '/a.js'; log('c');"),
    }
)

# import() from a classic script, a module and a string that a timer runs, against the document's base URL or the
# script's, with JSON and text modules, and how each failure rejects. The timer and the deferred module race, so the
# page logs them in a fixed order.
_CASES["dynamic_import"] = Case(
    {
        "/": _page(
            "<base href=/sub/>"
            "<script>"
            "const show = r => r.status === 'fulfilled' ? 'ok ' + JSON.stringify(r.value.default)"
            " : r.reason.name + ': ' + path(r.reason.message);"
            "window.later = {};"
            "window.cases = ["
            " ['relative', import('./x.js')],"
            " ['missing', import('./missing.js')],"
            " ['bare', import('bare')],"
            " ['json', import('./j.json', {with: {type: 'json'}})],"
            " ['text', import('./t.txt', {with: {type: 'text'}})],"
            " ['bad type', import('./j.json', {with: {type: 'xml'}})],"
            " ['bad key', import('./j.json', {with: {foo: 'x'}})],"
            " ['json as script', import('./j.json')],"
            " ['script as json', import('./x.js', {with: {type: 'json'}})],"
            " ['bad json', import('./bad.json', {with: {type: 'json'}})],"
            " ['throws', import('./throws.js')],"
            " ['syntax', import('./syntax.js')],"
            "];"
            "</script>"
            "<script type=module>"
            "window.later['from module'] = import('./x.js');"
            "window.later['meta resolve'] = Promise.resolve({default: import.meta.resolve('./y.js')});"
            "</script>"
            "<script>"
            "setTimeout(\"window.later['from timer'] = import('./x.js')\");"
            "addEventListener('load', () => setTimeout(async () => {"
            " for (const name of ['from module', 'meta resolve', 'from timer']) {"
            "  window.cases.push([name, window.later[name]]); }"
            " for (const [name, promise] of window.cases) {"
            "  const [result] = await Promise.allSettled([promise]); log(name + ' ' + show(result)); }"
            " finish(logged); }, 100));"
            "</script>"
        ),
        "/sub/x.js": _js("export default 'x';"),
        "/sub/j.json": Route('{"a": [1, 2]}', _JSON),
        "/sub/t.txt": Route("some text", "text/plain"),
        "/sub/bad.json": Route("{a: 1}", _JSON),
        "/sub/throws.js": _js("throw new Error('thrown');"),
        "/sub/syntax.js": _js("export default ;"),
    }
)

# What fails: a module that does not parse, throws, or whose graph fails to fetch or to link, and the events that each
# gives, at the window or at its element.
_CASES["errors"] = Case(
    {
        "/": _page(
            _FINISH_AFTER_LOAD + "<script>"
            "function events(id) { const s = document.getElementById(id);"
            " s.onload = () => log(id + ' load'); s.onerror = () => log(id + ' error'); }"
            "</script>"
            "<script type=module id=syntax src=/syntax.js></script><script>events('syntax')</script>"
            "<script type=module id=missing src=/missing.js></script><script>events('missing')</script>"
            "<script type=module id=plain src=/plain.js></script><script>events('plain')</script>"
            "<script type=module id=throws src=/throws.js></script><script>events('throws')</script>"
            "<script type=module id=deep src=/deep.js></script><script>events('deep')</script>"
            "<script type=module id=link src=/link.js></script><script>events('link')</script>"
            "<script type=module id=bare src=/bare.js></script><script>events('bare')</script>"
            "<script type=module id=inline>import '/missing.js'; log('inline ran');</script>"
            "<script>events('inline')</script>"
        ),
        "/syntax.js": _js("log('syntax ran'); export default ;"),
        "/plain.js": _js("log('plain ran');", content_type="text/plain"),
        "/throws.js": _js("log('throws ran'); throw new TypeError('thrown');"),
        "/deep.js": _js("import '/missing.js'; log('deep ran');"),
        "/link.js": _js("import {nothing} from '/empty.js'; log('link ran');"),
        "/empty.js": _js("log('empty ran');"),
        "/bare.js": _js("import 'lib'; log('bare ran');"),
    }
)

# Top-level await: the next deferred script and DOMContentLoaded do not wait for it, what imports it does, and a
# rejection is an error at the window.
_CASES["top_level_await"] = Case(
    {
        "/": _page(
            _FINISH_AFTER_LOAD + "<script>"
            "document.addEventListener('DOMContentLoaded', () => log('DOMContentLoaded ' + state()));"
            "</script>"
            "<script type=module id=m1 src=/importer.js></script>"
            "<script type=module id=m2>log('m2');</script>"
            "<script type=module id=m3>await Promise.resolve(); throw new Error('rejected');</script>"
        ),
        "/importer.js": _js("import '/waits.js'; log('importer');"),
        "/waits.js": _js(
            "log('waits start'); await new Promise(resolve => setTimeout(resolve, 50)); log('waits end');"
        ),
    }
)

# Import maps: bare specifiers, prefixes and scopes, a second map whose rule for a resolved specifier goes, one that
# does not parse, and one with src, which fires error.
_CASES["import_maps"] = Case(
    {
        "/": _page(
            _FINISH_AFTER_LOAD + '<script type=importmap>{"imports": {"lib": "/lib.js", "lib/": "/libdir/"},'
            ' "scopes": {"/scoped/": {"lib": "/lib2.js"}}}</script>'
            "<script type=module>"
            "import lib from 'lib'; import x from 'lib/x.js'; import scoped from '/scoped/s.js';"
            "log('lib ' + lib + ' ' + x + ' ' + scoped);"
            "log('resolve ' + path(import.meta.resolve('lib/y.js')));"
            "</script>"
            '<script type=importmap>{"imports": {"other": "/other.js", "lib": "/nope.js"}}</script>'
            "<script type=module>import other from 'other'; import lib from 'lib'; log('later ' + other + ' ' + lib);"
            "</script>"
            "<script type=importmap>{not json}</script>"
            "<script type=importmap src=/map.json onerror=\"log('src error')\"></script>"
        ),
        "/lib.js": _js("export default 'lib';"),
        "/lib2.js": _js("export default 'lib2';"),
        "/libdir/x.js": _js("export default 'x';"),
        "/scoped/s.js": _js("import lib from 'lib'; export default 'scoped ' + lib;"),
        "/other.js": _js("export default 'other';"),
        "/nope.js": _js("export default 'nope';"),
    }
)


# What Chrome 157 logged for each case.
_CHROME: dict[str, list[str]] = {
    "dynamic_import": [
        'relative ok "x"',
        "missing TypeError: Failed to fetch dynamically imported module: /sub/missing.js",
        "bare TypeError: Failed to resolve module specifier 'bare'",
        'json ok {"a":[1,2]}',
        'text ok "some text"',
        'bad type TypeError: "xml" is not a valid module type.',
        'bad key TypeError: Invalid attribute key "foo".',
        "json as script TypeError: Failed to fetch dynamically imported module: /sub/j.json",
        "script as json TypeError: Failed to fetch dynamically imported module: /sub/x.js",
        "bad json SyntaxError: Expected property name or '}' in JSON at position 1 (line 1 column 2)",
        "throws Error: thrown",
        "syntax SyntaxError: Unexpected token ';'",
        'from module ok "x"',
        'meta resolve ok "{origin}/sub/y.js"',
        'from timer ok "x"',
    ],
    "errors": [
        "window error Uncaught SyntaxError: Unexpected token ';' at /syntax.js:1",
        "syntax load",
        "missing error",
        "plain error",
        "throws ran",
        "window error Uncaught TypeError: thrown at /throws.js:1",
        "throws load",
        "deep error",
        "window error Uncaught SyntaxError: The requested module '/empty.js' does not provide an export named 'nothing' at /link.js:1",
        "link load",
        'window error Uncaught TypeError: Failed to resolve module specifier "lib". Relative references must start with either "/", "./", or "../". at /:0',
        "bare load",
        "inline error",
        "load complete null",
    ],
    "graph": [
        "c",
        "a",
        "b",
        "root b",
        "root load",
        "inline b",
        "load complete null",
    ],
    "import_maps": [
        "window error Uncaught SyntaxError: Failed to parse import map: invalid JSON at /:0",
        "src error",
        "lib lib x scoped lib2",
        "resolve /libdir/y.js",
        "later other lib",
        "load complete null",
    ],
    "order": [
        "c1 loading c1",
        "dep interactive null",
        "m1 interactive null",
        "m1 meta /m1.js",
        "d1 interactive d1",
        "i1 interactive null",
        "i1 meta /",
        "DOMContentLoaded interactive null",
        "a1 interactive null",
        "load complete null",
    ],
    "top_level_await": [
        "waits start",
        "m2",
        "window error Uncaught Error: rejected at /:1",
        "DOMContentLoaded interactive null",
        "load complete null",
        "waits end",
        "importer",
    ],
}


def _session(memory: int) -> mitto.Session:
    """Return a Session whose Machine has `memory` GiB, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    return mitto.Session(machine=mitto.Machine.draw(memory=memory))


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    with _session(8) as session:
        yield session


@pytest.fixture
def server() -> Iterator[ScriptServer]:
    with ScriptServer() as server:
        yield server


def _run(session: mitto.Session, server: ScriptServer, name: str) -> tuple[Page, list[str]]:
    """Open case `name`'s page and return the Page and what the page logged, with the server's origin as {origin}."""
    server.case = _CASES[name]
    page = session._open_page(server.url(), timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]
    logged: list[str] = page.evaluate("done", timeout=_TIMEOUT_SECONDS)
    origin = server.url("").rstrip("/")
    return page, [line.replace(origin, "{origin}") for line in logged]


@pytest.mark.parametrize("name", sorted(_CHROME))
def test_module_scripts_run_as_chrome_runs_them(session: mitto.Session, server: ScriptServer, name: str) -> None:
    page, logged = _run(session, server, name)
    with page:
        assert logged == _CHROME[name]
