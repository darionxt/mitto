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

# A module and a classic script of another origin, each of which imports more: what they import names them as its
# referrer, as Chrome sends it. The server's other name, localhost, is the other origin.
_CORS = [("Access-Control-Allow-Origin", "*")]
# A JSON module and a text module, each fetched once.
_CASES["module_lines"] = Case(
    {
        "/": _page(
            "<script type=module>"
            "import './m.json' with {type: 'json'};"
            "await import('./t.txt', {with: {type: 'text'}});"
            "log('imported'); finish(logged);"
            "</script>"
        ),
        "/m.json": Route("{}", _JSON),
        "/t.txt": Route("some text", "text/plain"),
    }
)

# A link with rel=modulepreload fetches a single module script into the module map, without running it, and fires load,
# or error where the fetch, its as or its integrity fails, whether from the integrity attribute or the import map's. One
# with no href, or whose media does not match, fetches nothing and fires nothing. What imports the module later takes it
# from the module map. The page logs each link's outcome in document order once they have come.
_MODULE_PRELOADS = ("js", "json", "missing", "invalid_as", "no_href", "integrity", "map_integrity", "media")
_CASES["modulepreload"] = Case(
    {
        "/": _page(
            '<script type=importmap>{"integrity": {"/map-integrity.js": "sha384-foobar"}}</script>'
            "<script>window.outcomes = {}; function outcome(link, x) { outcomes[link.id] = x; }</script>"
            + "".join(
                f"<link rel=modulepreload id={name} {attributes} onload=\"outcome(this, 'load')\""
                f" onerror=\"outcome(this, 'error')\">"
                for name, attributes in (
                    ("js", "href=/preloaded.js"),
                    ("json", "href=/preloaded.json as=json"),
                    ("missing", "href=/missing.js"),
                    ("invalid_as", "href=/invalid-as.js as=image"),
                    ("no_href", 'href=""'),
                    ("integrity", "href=/integrity.js integrity=sha384-foobar"),
                    ("map_integrity", "href=/map-integrity.js"),
                    ("media", 'href=/media.js media="(max-width: 1px)"'),
                )
            )
            + "<script>addEventListener('load', async () => {"
            "  for (let i = 0; i < 100 && Object.keys(outcomes).length < 6; ++i) {"
            "    await new Promise(resolve => setTimeout(resolve, 20));"
            "  }"
            f"  for (const name of {list(_MODULE_PRELOADS)}) log(name + ' ' + (outcomes[name] || 'none'));"
            "  await import('/preloaded.js');"
            "  const json = await import('/preloaded.json', {with: {type: 'json'}});"
            "  log('json ' + JSON.stringify(json.default));"
            "  finish(logged);"
            "});</script>"
        ),
        "/preloaded.js": _js("log('preloaded ran');"),
        "/preloaded.json": Route('{"a": 1}', _JSON),
        "/missing.js": _js("", status=404),
        "/invalid-as.js": _js("log('invalid as ran');"),
        "/integrity.js": _js("log('integrity ran');"),
        "/map-integrity.js": _js("log('map integrity ran');"),
        "/media.js": _js("log('media ran');"),
    }
)

# Module preloads that a script inserts, which Chrome's preload scanner never sees, so that the requests are the links'
# own: a JavaScript module's, and a JSON module's by its as.
_CASES["modulepreload_lines"] = Case(
    {
        "/": _page(
            "<script>"
            "Promise.all(["
            "  ['/inserted.js', 'script'], ['/inserted.json', 'json']"
            "].map(([href, as]) => new Promise(resolve => {"
            "  const link = Object.assign(document.createElement('link'), {rel: 'modulepreload', href, as});"
            "  link.onload = () => resolve(as + ' load'); link.onerror = () => resolve(as + ' error');"
            "  document.head.append(link);"
            "}))).then(outcomes => { outcomes.forEach(log); finish(logged); });"
            "</script>"
        ),
        "/inserted.js": _js("log('inserted ran');"),
        "/inserted.json": Route("{}", _JSON),
    }
)

# The document's referrer policy, from a meta element, which a module script, what it imports and a module preload go
# with where their own policy is the empty string.
_CASES["module_document_policy"] = Case(
    {
        "/": _page(
            "<meta name=referrer content=origin>"
            "<link rel=modulepreload href=/policy/preloaded.js>"
            "<script type=module src=/policy/root.js></script>" + _FINISH_AFTER_LOAD
        ),
        "/policy/root.js": _js("import './imported.js'; await import('./preloaded.js'); log('root');"),
        "/policy/imported.js": _js("log('imported');"),
        "/policy/preloaded.js": _js("log('preloaded');"),
    }
)

_CASES["referrers"] = Case(
    {
        "/": _page(
            "<script>"
            "window.pending = 2;"
            "function settled(x) { log(x); if (--pending === 0) finish(logged); }"
            "const remote = 'http://localhost:' + location.port + '/remote/';"
            "document.head.append(Object.assign(document.createElement('script'),"
            " {type: 'module', src: remote + 'root.js'}));"
            "document.head.append(Object.assign(document.createElement('script'),"
            " {crossOrigin: 'anonymous', src: remote + 'classic.js'}));"
            "</script>"
        ),
        "/remote/root.js": Route(
            "import './static.js'; await import('./dynamic.js'); settled('module');", _JAVASCRIPT, headers=_CORS
        ),
        "/remote/static.js": Route("", _JAVASCRIPT, headers=_CORS),
        "/remote/dynamic.js": Route("", _JAVASCRIPT, headers=_CORS),
        "/remote/classic.js": Route(
            "import('./from-classic.js').then(() => settled('classic'));", _JAVASCRIPT, headers=_CORS
        ),
        "/remote/from-classic.js": Route("", _JAVASCRIPT, headers=_CORS),
    }
)

# Module scripts and imports of data: URLs, which a page answers itself, with no request.
_CASES["data_urls"] = Case(
    {
        "/": _page(
            "<script type=module src=\"data:text/javascript,log('static ' + document.readyState)\"></script>"
            "<script>"
            "const show = r => r.status === 'fulfilled' ? 'ok ' + JSON.stringify(r.value.default) : r.reason.name;"
            "addEventListener('load', () => setTimeout(async () => {"
            " for (const [name, promise] of ["
            "  ['javascript', import('data:text/javascript,export default 42')],"
            "  ['json', import('data:application/json,{\"a\":1}', {with: {type: 'json'}})],"
            "  ['base64', import('data:text/javascript;base64,ZXhwb3J0IGRlZmF1bHQgJ2I2NCc7')],"
            "  ['wrong type', import('data:text/plain,export default 1')],"
            "  ['relative', import('data:text/javascript,import \"./x.js\"')],"
            "  ['no comma', import('data:text/javascript')],"
            " ]) { const [result] = await Promise.allSettled([promise]); log(name + ' ' + show(result)); }"
            " finish(logged); }, 100));"
            "</script>"
        ),
    }
)

# Modules whose URLs differ only in their fragments. While the document loads, Chrome fetches root.js once for all three,
# as its document's resources do, and each module takes the first response's URL. late.js answers only after the load
# event, so its second import comes after it and fetches again, with its own URL.
_CASES["fragments"] = Case(
    {
        "/": _page(
            "<script type=module src=/fragments/main.js></script>"
            "<script>addEventListener('load', () => { log('load'); mark('after-load'); });</script>"
        ),
        "/fragments/main.js": _js(
            "import {m as a} from './root.js'; import {m as b} from './root.js#1';"
            "import {m as c} from './root.js#2';"
            "for (const m of [a, b, c]) log(path(m.url));"
            "const d = await import('./late.js#x'); log(path(d.m.url));"
            "const e = await import('./late.js'); log(path(e.m.url)); finish(logged);"
        ),
        "/fragments/root.js": _js("export const m = import.meta;"),
        "/fragments/late.js": _js("export const m = import.meta;", hold="after-load"),
    }
)

# A module that imports what the document has fetched while it loads: a link preload and a classic script of mode cors,
# which it takes, and a classic script of mode no-cors, which it fetches again.
_CASES["module_takes_resources"] = Case(
    {
        "/": _page(
            "<link rel=preload as=script crossorigin href=/taken/preloaded.js>"
            "<script crossorigin src=/taken/classic.js></script>"
            "<script src=/taken/no-cors.js></script>"
            "<script type=module src=/taken/main.js></script>" + _FINISH_AFTER_LOAD
        ),
        "/taken/preloaded.js": _js("log('preloaded ran');"),
        "/taken/classic.js": _js("log('classic ran');"),
        "/taken/no-cors.js": _js("log('no-cors ran');"),
        "/taken/main.js": _js("import './preloaded.js'; import './classic.js'; import './no-cors.js'; log('main');"),
    }
)

# What Chrome 157 logged for each case.
_CHROME: dict[str, list[str]] = {
    "data_urls": [
        "static interactive",
        "javascript ok 42",
        'json ok {"a":1}',
        'base64 ok "b64"',
        "wrong type TypeError",
        "relative TypeError",
        "no comma TypeError",
    ],
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
    "fragments": [
        "/fragments/root.js",
        "/fragments/root.js",
        "/fragments/root.js",
        "load",
        "/fragments/late.js#x",
        "/fragments/late.js",
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
    "module_document_policy": ["imported", "load complete null", "preloaded", "root"],
    "module_takes_resources": [
        "classic ran",
        "no-cors ran",
        "preloaded ran",
        "classic ran",
        "no-cors ran",
        "main",
        "load complete null",
    ],
    "modulepreload": [
        "js load",
        "json load",
        "missing error",
        "invalid_as error",
        "no_href none",
        "integrity error",
        "map_integrity error",
        "media none",
        "preloaded ran",
        'json {"a":1}',
    ],
    "modulepreload_lines": ["script load", "json load"],
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


# The Referer that Chrome 157 sent for each request of the referrers case, with the server's origins written short.
_CHROME_REFERERS = {
    "/remote/root.js": "{origin}/",
    "/remote/classic.js": "{origin}/",
    "/remote/static.js": "{other}/remote/root.js",
    "/remote/dynamic.js": "{other}/remote/root.js",
    "/remote/from-classic.js": "{other}/remote/classic.js",
}


def test_what_a_script_of_another_origin_imports_names_it_as_the_referrer(
    session: mitto.Session, server: ScriptServer
) -> None:
    # The Session takes such a referrer as the final URL of a response that came whole to the document.
    page, logged = _run(session, server, "referrers")
    origin = server.url("").rstrip("/")
    other = origin.replace("127.0.0.1", "localhost")
    with page:
        assert sorted(logged) == ["classic", "module"]
        assert {
            path: (referer or "").replace(origin, "{origin}").replace(other, "{other}")
            for path, referer in server.referers.items()
            if path.startswith("/remote/")
        } == _CHROME_REFERERS


def test_a_json_module_sends_the_accept_and_sec_fetch_dest_of_its_destination(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "module_lines")
    with page:
        assert logged == ["imported"]
        lines = {path: (h.get("accept"), h.get("sec-fetch-dest")) for path, h in server.request_headers.items()}
        # As Windows Chrome's Baselines have module_json.
        assert lines["/m.json"] == ("application/json,*/*;q=0.5", "json")
        # Chrome sends text/plain,*/*;q=0.5 and text, which a text module gets once Chrome 155, which has no text
        # modules, leaves the Supported Window: until then it goes as a module, a Known Gap.
        assert lines["/t.txt"] == ("*/*", "script")


def test_a_module_preload_sends_the_lines_of_its_module_type_and_runs_nothing(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "modulepreload_lines")
    with page:
        assert logged == _CHROME["modulepreload_lines"]
        lines = {
            path: (h.get("accept"), h.get("sec-fetch-dest"), h.get("sec-fetch-mode"), h.get("referer"))
            for path, h in server.request_headers.items()
        }
        # What Chrome 157 sent for links that a script inserted, with the document as the referrer.
        page_url = server.url()
        assert lines["/inserted.js"] == ("*/*", "script", "cors", page_url)
        assert lines["/inserted.json"] == ("application/json,*/*;q=0.5", "json", "cors", page_url)


def test_a_module_preload_fetches_each_module_once_for_what_imports_it_later(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, _ = _run(session, server, "modulepreload")
    with page:
        requested = [path for path in server.requests if not path.startswith(("/mark/", "/result"))]
        assert requested.count("/preloaded.js") == 1
        assert requested.count("/preloaded.json") == 1
        # An as that names no module type, and a media that does not match, fetch nothing from the link. Chrome's
        # preload scanner, which a Page does not have, fetches a link in the markup whatever its as: a Known Gap.
        assert "/invalid-as.js" not in requested
        assert "/media.js" not in requested


def test_modules_whose_urls_differ_in_fragments_take_one_fetch_while_the_document_loads(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "fragments")
    with page:
        assert logged == _CHROME["fragments"]
        # As Chrome 157 sent them: root.js once for its three imports, and late.js twice, once before the load event
        # and once after it.
        requested = [path for path in server.requests if path.startswith("/fragments/")]
        assert requested.count("/fragments/root.js") == 1
        assert requested.count("/fragments/late.js") == 2


def test_a_module_takes_a_preload_and_a_classic_script_of_its_modes_while_the_document_loads(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "module_takes_resources")
    with page:
        assert logged == _CHROME["module_takes_resources"]
        # As Chrome 157 sent them: the preload and the classic script of mode cors once, and the classic script of mode
        # no-cors once for itself and once for the module.
        requested = [path for path in server.requests if path.startswith("/taken/")]
        assert requested.count("/taken/preloaded.js") == 1
        assert requested.count("/taken/classic.js") == 1
        assert requested.count("/taken/no-cors.js") == 2


def test_a_module_script_its_imports_and_a_module_preload_go_with_the_documents_referrer_policy(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "module_document_policy")
    with page:
        assert logged == _CHROME["module_document_policy"]
        # As Chrome 157 sent them, by the meta element's origin policy: what root.js imports names the origin, where
        # the default policy names root.js.
        assert {
            path: referer for path, referer in server.referers.items() if path.startswith("/policy/")
        } == dict.fromkeys(("/policy/preloaded.js", "/policy/root.js", "/policy/imported.js"), server.url())
