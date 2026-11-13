"""Tests of when a Page runs a document's scripts: parser-blocking, deferred and async ones, those that the DOM inserts,
their types, load and error, document.currentScript, <base>, Subresource Integrity, and the task priorities of Blink's
scheduler that decide which runs first.

Each case is a page that logs what runs, in order, and a local server that holds a response until the page sends a
mark, fetch('/mark/NAME'), so that the order does not hang on the network's timing. The orders are what Chrome
157.0.8103.51 gave by hand, headless on macOS against the same pages and server. Each test goes through both APIs, and
needs a native library with the Page program beside it, as CI's native job installs it on every Host.
"""

import base64
import hashlib
import http.server
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

_JAVASCRIPT = "text/javascript"
_HTML = "text/html; charset=utf-8"
_HTML_TYPE = "text/html"

# What every case's page starts with: `log()` and `finish()`, which settles `done` with what was logged, a mark that
# releases what the server holds, and what document.currentScript and readyState are, written short.
_PRELUDE = (
    "<!doctype html><script>"
    "window.logged = [];"
    "function log(x) { logged.push(String(x)); }"
    "window.done = new Promise(resolve => { window.finish = resolve; });"
    "done.then(l => fetch('/result', {method: 'POST', body: JSON.stringify(l)}));"
    "setTimeout(() => { log('gave up'); finish(logged); }, 5000);"
    "function mark(name) { fetch('/mark/' + name); }"
    "function current() { const s = document.currentScript;"
    " return s === null ? 'null' : s.id || (s.src ? s.src.slice(s.src.lastIndexOf('/') + 1) : 'anonymous'); }"
    "function state() { return document.readyState + ' ' + current(); }"
    "</script>"
)


@dataclass
class _Route:
    """What the server answers for one path.

    With `hold`, the answer waits for that mark. With `pieces`, the body goes out piece by piece, each once the marks
    that it names, separated by commas, have come, and the connection closes at the end.
    """

    body: str = ""
    content_type: str = _JAVASCRIPT
    status: int = 200
    headers: list[tuple[str, str]] = field(default_factory=list[tuple[str, str]])
    hold: str | None = None
    pieces: list[tuple[str | None, str]] | None = None


@dataclass
class _Case:
    routes: dict[str, _Route]


def _page(markup: str, pieces: list[tuple[str | None, str]] | None = None) -> _Route:
    if pieces is not None:
        return _Route(content_type=_HTML, pieces=[(None, _PRELUDE + markup), *pieces])
    return _Route(_PRELUDE + markup, _HTML)


def _js(source: str, hold: str | None = None, status: int = 200) -> _Route:
    return _Route(source, _JAVASCRIPT, status, hold=hold)


_CASES: dict[str, _Case] = {}

# Parser-blocking, deferred and async scripts, with the timers and microtasks they queue, readiness and its events.
_CASES["parser_order"] = _Case(
    {
        "/": _page(
            "<script>"
            "document.addEventListener('readystatechange', () => log('readystatechange ' + document.readyState));"
            "document.addEventListener('DOMContentLoaded', () => {"
            " log('DOMContentLoaded ' + state()); setTimeout(() => log('timer from DOMContentLoaded')); mark('dcl'); });"
            "addEventListener('load', () => { log('load ' + state()); setTimeout(() => finish(logged), 100); });"
            "</script>"
            "<script id=i1>log('i1 ' + state()); setTimeout(() => { log('timer from i1'); mark('i1'); });</script>"
            "<script id=d1 defer src=/d1.js></script>"
            "<script id=a1 async src=/a1.js></script>"
            "<script id=b1 src=/b1.js></script>"
            "<script id=d2 defer src=/d2.js></script>"
            "<script id=i2>log('i2 ' + state())</script>",
            pieces=[("served:/d1.js,served:/d2.js", "<p>end</p>")],
        ),
        "/d1.js": _js("log('d1 ' + state()); setTimeout(() => log('timer from d1'));"),
        "/d2.js": _js("log('d2 ' + state()); Promise.resolve().then(() => log('microtask from d2'));"),
        "/a1.js": _js("log('a1 ' + state());", hold="dcl"),
        "/b1.js": _js(
            "log('b1 ' + state()); setTimeout(() => log('timer from b1'));"
            "Promise.resolve().then(() => log('microtask from b1'));",
            hold="i1",
        ),
    }
)

# Async scripts run as each arrives: during parsing, and after DOMContentLoaded, and load waits for them.
_CASES["async_order"] = _Case(
    {
        "/": _page(
            "<script>"
            "document.addEventListener('DOMContentLoaded', () => { log('DOMContentLoaded'); mark('dcl'); });"
            "addEventListener('load', () => { log('load'); setTimeout(() => finish(logged), 100); });"
            "</script>"
            "<script id=a1 async src=/a1.js></script>"
            "<script id=a2 async src=/a2.js></script>"
            "<script>log('inline ' + state())</script>",
            pieces=[("a2", "<script>log('after a2 ' + state())</script><p>end</p>")],
        ),
        "/a1.js": _js("log('a1 ' + state());", hold="dcl"),
        "/a2.js": _js("log('a2 ' + state()); mark('a2');"),
    }
)

# DOMContentLoaded, complete and load once the body ends, against timers.
_CASES["readiness"] = _Case(
    {
        "/": _page(
            "<script>"
            "document.addEventListener('readystatechange', () => {"
            " const state = document.readyState; log('readystatechange ' + state); setTimeout(() => log('timer from ' + state)); });"
            "document.addEventListener('DOMContentLoaded', () => {"
            " log('DOMContentLoaded'); setTimeout(() => log('timer from DOMContentLoaded'));"
            " Promise.resolve().then(() => log('microtask from DOMContentLoaded')); });"
            "addEventListener('load', () => { log('load'); setTimeout(() => finish(logged), 100); });"
            "setTimeout(() => { log('timer from inline'); mark('timer'); });"
            "</script><p>text</p>",
            pieces=[("timer", "")],
        ),
    }
)

# Scripts that the DOM inserts: inline ones at once, external ones as soon as each arrives, async=false in order.
_CASES["inserted_order"] = _Case(
    {
        "/": _page(
            "<script>"
            "function insert(id, src, inOrder) {"
            " const s = document.createElement('script'); s.id = id; s.src = src;"
            " if (inOrder) { s.async = false; }"
            " s.addEventListener('load', () => log(id + ' load'));"
            " document.head.append(s); return s; }"
            "document.addEventListener('DOMContentLoaded', () => {"
            " const x = document.createElement('script'); x.id = 'x1';"
            " x.textContent = \"log('x1 ' + state())\";"
            " log('x1 async ' + x.async); document.body.append(x); log('after x1 ' + state());"
            " insert('e1', '/e1.js'); insert('e2', '/e2.js');"
            " insert('o1', '/o1.js', true); insert('o2', '/o2.js', true);"
            " const s = document.createElement('script'); s.async = false; const before = s.async;"
            " s.setAttribute('async', ''); log('async ' + before + ' ' + s.async);"
            " log('parsed async ' + document.getElementById('p1').async + ' ' + document.getElementById('p2').async);"
            "});"
            "addEventListener('load', () => { log('load'); setTimeout(() => finish(logged), 100); });"
            "</script>"
            "<script id=p1>log('p1')</script><script id=p2 async src=/p2.js></script>"
        ),
        "/p2.js": _js(""),
        "/e1.js": _js("log('e1 ' + state()); mark('e1');", hold="e2"),
        "/e2.js": _js("log('e2 ' + state()); mark('e2');"),
        "/o1.js": _js("log('o1 ' + state());", hold="e1"),
        "/o2.js": _js("log('o2 ' + state());"),
    }
)

# When an inserted script runs, and when it does not.
_CASES["inserted_cases"] = _Case(
    {
        "/": _page(
            "<body><script>"
            "function script(id, text) { const s = document.createElement('script'); s.id = id;"
            " if (text !== undefined) { s.textContent = text; } return s; }"
            "addEventListener('load', () => {"
            # Text added to an empty script that is connected runs it, once.
            " const a = script('a'); document.body.append(a); log('a inserted');"
            " a.textContent = \"log('a ran ' + state())\"; log('a text set');"
            " a.textContent = \"log('a ran again')\"; a.append('log(\"a appended\")');"
            # A src set on an empty script that is connected fetches it.
            " const b = script('b'); document.body.append(b);"
            " b.addEventListener('load', () => { log('b load'); setTimeout(() => finish(logged), 100); }); b.src = '/b.js';"
            " log('b src set');"
            # A script that has run does not run again when it is inserted again, nor do its clones.
            " const c = script('c', \"log('c ran')\"); document.body.append(c); c.remove(); document.body.append(c);"
            " document.body.append(c.cloneNode(true));"
            # A clone of a script that has not run runs, and so does the script.
            " const e = script('e', \"log('e ran ' + current())\"); const clone = e.cloneNode(true); clone.id = 'e-clone';"
            " document.body.append(clone); document.body.append(e);"
            # innerHTML does not run scripts.
            " const d = document.createElement('div'); d.innerHTML = '<script>log(\"d ran\")<\\/script>';"
            " document.body.append(d); document.body.append(d.firstChild);"
            # The first of two scripts that one call inserts runs once both are connected.
            " document.body.append(script('f1', \"log('f1 sees f2 ' + (document.getElementById('f2') !== null))\"),"
            "  script('f2', \"log('f2 ran')\"));"
            # A data block can run later, once its type is a script's.
            " const h = script('h', \"log('h ran')\"); h.type = 'text/plain'; document.body.append(h);"
            " h.remove(); h.type = ''; document.body.append(h);"
            # A script inserted by a script runs inside it.
            " const n = script('n', \"log('n outer ' + current());"
            "  const inner = document.createElement('script'); inner.id = 'n-inner';"
            "  inner.textContent = \\\"log('n inner ' + current())\\\"; document.body.append(inner);"
            "  log('n outer again ' + current())\"); document.body.append(n);"
            " log('current after ' + current());"
            "});"
            "</script></body>"
        ),
        "/b.js": _js("log('b ran ' + current());"),
    }
)

# A script that leaves the document, or goes to another, while it is fetched.
_CASES["inserted_moved"] = _Case(
    {
        "/": _page(
            "<script>"
            "addEventListener('load', () => {"
            " const i = document.createElement('script'); i.id = 'i'; i.src = '/i.js';"
            " i.addEventListener('load', () => { log('i load connected ' + i.isConnected);"
            "  setTimeout(() => finish(logged), 200); });"
            " i.addEventListener('error', () => log('i error'));"
            " document.head.append(i); i.remove();"
            " mark('go'); log('marked');"
            "});"
            "</script>"
        ),
        "/i.js": _js("log('i ran ' + current());", hold="go"),
    }
)

# Which types and languages run, nomodule, event and for, and HTMLScriptElement.supports().
_TYPES = [
    ("no type", ""),
    ("empty type", "type=''"),
    ("text/javascript", "type=text/javascript"),
    ("spaced and in capitals", "type=' TEXT/JavaScript '"),
    ("javascript1.5", "type=text/javascript1.5"),
    ("application/ecmascript", "type=application/ecmascript"),
    ("x-javascript", "type=application/x-javascript"),
    ("jscript", "type=text/jscript"),
    ("livescript", "type=text/livescript"),
    ("with a parameter", "type='text/javascript; charset=utf-8'"),
    ("json", "type=application/json"),
    ("plain", "type=text/plain"),
    ("unknown", "type=text/x-unknown"),
    ("language javascript", "language=javascript"),
    ("language JavaScript1.2", "language=JavaScript1.2"),
    ("language empty", "language=''"),
    ("language vbscript", "language=vbscript"),
    ("type wins over language", "type=text/javascript language=vbscript"),
    ("empty type with language vbscript", "type='' language=vbscript"),
    ("nomodule", "nomodule"),
    ("for window event onload", "for=window event=onload"),
    ("for window event onload()", "for=' Window ' event=' onload() '"),
    ("for document event onload", "for=document event=onload"),
    ("for window event onclick", "for=window event=onclick"),
    ("event alone", "event=onclick"),
    ("for alone", "for=document"),
]
_CASES["script_types"] = _Case(
    {
        "/": _page(
            "".join(f"<script {attributes}>log({label!r})</script>" for label, attributes in _TYPES) + "<script>"
            "log(['classic', 'module', 'importmap', 'speculationrules', 'webbundle', 'text/javascript', '', 'json',"
            " 'CLASSIC', ' classic'].map(type => type + ' ' + HTMLScriptElement.supports(type)).join(', '));"
            "addEventListener('load', () => finish(logged));"
            "</script>"
        ),
    }
)

# load and error at scripts that the DOM inserts, in order, with what Chrome runs and refuses.
_EVENTS = [
    ("ok", "/ok.js"),
    ("not found", "/missing.js"),
    ("server error", "/error.js"),
    ("text/html", "/html.js"),
    ("text/plain", "/plain.js"),
    ("text/plain with nosniff", "/plain-nosniff.js"),
    ("image/png", "/image.js"),
    ("text/csv", "/csv.js"),
    ("no type", "/untyped.js"),
    ("redirected", "/redirect.js"),
]
_CASES["load_and_error"] = _Case(
    {
        "/": _page(
            "<script>"
            "function insert(id, src, attributes) {"
            " const s = document.createElement('script'); s.id = id; s.async = false;"
            " for (const [name, value] of Object.entries(attributes || {})) { s.setAttribute(name, value); }"
            " if (src !== null) { s.setAttribute('src', src); }"
            " s.addEventListener('load', () => log(id + ' load ' + current()));"
            " s.addEventListener('error', e => log(id + ' error ' + current() + ' ' + e.bubbles + ' ' + e.cancelable));"
            " document.head.append(s); return s; }"
            "addEventListener('load', () => {"
            + "".join(f" insert({label!r}, {src!r});" for label, src in _EVENTS)
            + " insert('empty src', ''); insert('spaces', '  '); insert('invalid URL', 'http://[');"
            " insert('last', '/last.js');"
            " log('inserted');"
            "});"
            "</script>"
        ),
        "/ok.js": _js("log('ok ran ' + current()); Promise.resolve().then(() => log('microtask from ok'));"),
        "/missing.js": _js("log('not found ran')", status=404),
        "/error.js": _js("log('server error ran')", status=500),
        "/html.js": _Route("log('text/html ran')", _HTML_TYPE),
        "/plain.js": _Route("log('text/plain ran')", "text/plain"),
        "/plain-nosniff.js": _Route(
            "log('text/plain with nosniff ran')", "text/plain", headers=[("X-Content-Type-Options", "nosniff")]
        ),
        "/image.js": _Route("log('image/png ran')", "image/png"),
        "/csv.js": _Route("log('text/csv ran')", "text/csv"),
        "/untyped.js": _Route("log('no type ran')", ""),
        "/redirect.js": _Route(status=302, headers=[("Location", "/ok2.js")]),
        "/ok2.js": _js("log('redirected ran ' + current())"),
        "/last.js": _js("log('last ran'); setTimeout(() => finish(logged), 100);"),
    }
)

# <base> for the URLs that a document resolves.
_CASES["base_url"] = _Case(
    {
        "/": _page(
            "<base id=b0><base id=b1 href='  /sub/dir/  '><base id=b2 href='/other/'>"
            "<script id=b src=b.js></script>"
            "<script>"
            "function attempt(f) { try { f(); } catch (e) { log('threw ' + e); } }"
            "attempt(() => log('baseURI ' + document.baseURI));"
            "attempt(() => log('script src ' + document.getElementById('b').src));"
            "attempt(() => log('request ' + new Request('r').url));"
            "const bases = ['b0', 'b1', 'b2'].map(id => document.getElementById(id));"
            "attempt(() => log('base href ' + bases.map(b => b.href).join(' ')));"
            "fetch('f.json').then(r => r.text()).catch(e => 'threw ' + e).then(t => { attempt(() => {"
            " log('fetch ' + t);"
            " bases[1].remove();"
            " log('after remove ' + document.baseURI);"
            " bases[2].setAttribute('href', 'http://[');"
            " log('invalid ' + document.baseURI + ' ' + bases[2].href);"
            " attempt(() => log('invalid request ' + new Request('r').url));"
            " attempt(() => log('invalid absolute request ' + new Request('/r').url));"
            " bases[2].setAttribute('href', '');"
            " log('empty ' + document.baseURI + ' ' + bases[2].href);"
            " bases[2].removeAttribute('href');"
            " log('none ' + document.baseURI + ' ' + bases[2].href);"
            " const late = document.createElement('base'); late.href = '/late/'; document.body.append(late);"
            " log('late ' + document.baseURI + ' ' + late.href);"
            " const s = document.createElement('script'); s.src = 'l.js'; s.async = false;"
            " s.addEventListener('load', () => finish(logged)); s.addEventListener('error', () => finish(logged));"
            " document.head.append(s);"
            "}); });"
            "</script>"
        ),
        "/sub/dir/b.js": _js("log('b ' + current());"),
        "/sub/dir/f.json": _Route("sub", "application/json"),
        "/late/l.js": _js("log('l ' + document.currentScript.src);"),
    }
)

# document.currentScript where it is set, and where it is null.
_CASES["current_script"] = _Case(
    {
        "/": _page(
            "<script id=s1>"
            "log('inline ' + current());"
            "Promise.resolve().then(() => log('microtask ' + current()));"
            "setTimeout(() => log('timer ' + current()));"
            "document.addEventListener('DOMContentLoaded', () => { log('listener ' + current()); mark('dcl'); });"
            "addEventListener('load', () => { log('load ' + current()); setTimeout(() => finish(logged), 100); });"
            "</script>"
            "<script id=s2 src=/s2.js></script>"
            "<script id=s3 defer src=/s3.js></script>"
            "<script id=s4 async src=/s4.js></script>"
            "<script>log('anonymous inline ' + current() + ' ' + document.currentScript.textContent.startsWith('log('))</script>"
        ),
        "/s2.js": _js("log('external ' + current());"),
        "/s3.js": _js("log('deferred ' + current());"),
        "/s4.js": _js("log('async ' + current());", hold="dcl"),
    }
)


def _digest(algorithm: str, text: str, urlsafe: bool = False) -> str:
    """Return the integrity metadata of `text` for `algorithm`, as base64 or base64url."""
    raw = hashlib.new(algorithm, text.encode()).digest()
    encoded = (base64.urlsafe_b64encode(raw) if urlsafe else base64.b64encode(raw)).decode()
    return f"{algorithm}-{encoded}"


_SRI_SOURCE = "log(document.currentScript.id + ' ran');"
_WRONG = "log('something else');"
_INTEGRITY = [
    ("sha256", _digest("sha256", _SRI_SOURCE)),
    ("sha384", _digest("sha384", _SRI_SOURCE)),
    ("sha512", _digest("sha512", _SRI_SOURCE)),
    ("wrong sha384", _digest("sha384", _WRONG)),
    ("unknown algorithm", "md5-" + base64.b64encode(hashlib.md5(_SRI_SOURCE.encode()).digest()).decode()),
    ("empty digest", "sha384-"),
    ("not base64", "sha384-***"),
    ("strongest matches", _digest("sha256", _WRONG) + " " + _digest("sha512", _SRI_SOURCE)),
    ("strongest does not", _digest("sha256", _SRI_SOURCE) + " " + _digest("sha512", _WRONG)),
    ("one of two of a kind", _digest("sha384", _WRONG) + "\t" + _digest("sha384", _SRI_SOURCE)),
    ("base64url", _digest("sha384", _SRI_SOURCE, urlsafe=True)),
    ("with options", _digest("sha384", _SRI_SOURCE) + "?foo=bar"),
    ("in capitals", _digest("sha384", _SRI_SOURCE).replace("sha384", "SHA384")),
    ("spaces only", "   "),
    ("wrong in capitals", _digest("sha384", _WRONG).replace("sha384", "SHA384")),
    ("wrong base64url", _digest("sha384", _WRONG, urlsafe=True)),
    ("wrong with options", _digest("sha384", _WRONG) + "?foo=bar"),
    ("wrong without padding", _digest("sha384", _WRONG).rstrip("=")),
]
_CASES["integrity"] = _Case(
    {
        "/": _page(
            "<script>"
            "function insert(id, src, integrity) {"
            " const s = document.createElement('script'); s.id = id; s.async = false; s.src = src;"
            " if (integrity !== null) { s.integrity = integrity; }"
            " s.addEventListener('load', () => log(id + ' load'));"
            " s.addEventListener('error', () => log(id + ' error'));"
            " document.head.append(s); return s; }"
            "addEventListener('load', () => {"
            + "".join(f" insert({label!r}, '/sri.js', {value!r});" for label, value in _INTEGRITY)
            + " const other = 'http://localhost:' + location.port + '/sri.js';"
            f" insert('cross-origin', other, {_digest('sha384', _SRI_SOURCE)!r});"
            " insert('cross-origin without integrity', other, null);"
            " insert('last', '/last.js', null);"
            "});"
            "</script>"
            f"<script id=parsed-wrong integrity={_digest('sha384', _WRONG)!r} src=/sri.js></script>"
            f"<script id=parsed-right integrity={_digest('sha384', _SRI_SOURCE)!r} src=/sri.js></script>"
            f"<script integrity={_digest('sha384', _WRONG)!r}>log('inline with a wrong digest ran')</script>"
        ),
        "/sri.js": _js(_SRI_SOURCE),
        "/last.js": _js("log('last ran'); setTimeout(() => finish(logged), 100);"),
    }
)

# Chrome runs an async script of another site at a low priority while a page loads: after the tasks before it.
_CASES["cross_site_async"] = _Case(
    {
        "/": _page(
            "<script>"
            "let running = false;"
            "function insert(id, src) { const s = document.createElement('script'); s.id = id; s.src = src;"
            " document.head.append(s); }"
            "insert('cross-site', 'http://localhost:' + location.port + '/x.js');"
            "insert('same-site', '/y.js');"
            "let left = 40;"
            "function step() { const end = performance.now() + 10; while (performance.now() < end) {}"
            " if (--left > 0) { scheduler.postTask(step); } else { running = false; log('chain end'); } }"
            "scheduler.postTask(() => { running = true; log('chain start'); mark('go'); step(); });"
            "let ran = 0;"
            "function oneRan() { if (++ran === 2) { setTimeout(() => finish(logged), 100); } }"
            "</script>"
        ),
        "/x.js": _js("log('cross-site ran while the chain runs ' + running); oneRan();", hold="go"),
        "/y.js": _js("log('same-site ran while the chain runs ' + running); oneRan();", hold="go"),
    }
)


class _BacklogServer(QuietServer):
    """A local server that queues as many connections as a page opens at once.

    A case such as `integrity` opens a connection for each of some twenty scripts, and socketserver's backlog of five
    refuses the rest on a busy macOS runner, which a script reports as an error.
    """

    request_queue_size = 128


class _ScriptServer:
    """A local server for one case at a time, which records the path of each request it gets, in order.

    A request for /mark/NAME sets the mark NAME, and sending a route's whole response sets served:PATH.
    """

    def __init__(self) -> None:
        self.case = _Case({})
        self.requests: list[str] = []
        self._marks: dict[str, threading.Event] = {}
        self._lock = threading.Lock()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self) -> None:
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                server.answer(self, 200, "text/plain", b"ok")

            def do_GET(self) -> None:
                server.serve(self)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = _BacklogServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def mark(self, name: str) -> threading.Event:
        with self._lock:
            return self._marks.setdefault(name, threading.Event())

    def answer(self, handler: http.server.BaseHTTPRequestHandler, status: int, content_type: str, body: bytes) -> None:
        handler.send_response(status)
        handler.send_header("Content-Type", content_type)
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def serve(self, handler: http.server.BaseHTTPRequestHandler) -> None:
        path = handler.path
        with self._lock:
            self.requests.append(path)
        if path.startswith("/mark/"):
            self.mark(path.removeprefix("/mark/")).set()
            self.answer(handler, 200, "text/plain", b"ok")
            return
        route = self.case.routes.get(path)
        if route is None:
            self.answer(handler, 404, "text/plain", b"not here")
            return
        if route.hold:
            self.mark(route.hold).wait(_TIMEOUT_SECONDS)
        served = self.mark(f"served:{path}")
        if route.pieces is not None:
            handler.send_response(route.status)
            handler.send_header("Content-Type", route.content_type)
            handler.send_header("Connection", "close")
            handler.end_headers()
            for hold, piece in route.pieces:
                if hold:
                    for name in hold.split(","):
                        self.mark(name).wait(_TIMEOUT_SECONDS)
                    # As long as the run in Chrome gave it to take what came before.
                    time.sleep(0.2)
                handler.wfile.write(piece.encode())
                handler.wfile.flush()
            handler.close_connection = True
            return
        handler.send_response(route.status)
        if route.content_type:
            handler.send_header("Content-Type", route.content_type)
        for name, value in route.headers:
            handler.send_header(name, value)
        body = route.body.encode()
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
        handler.wfile.flush()
        served.set()

    def __enter__(self) -> "_ScriptServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_ScriptServer]:
    with _ScriptServer() as script_server:
        yield script_server


def _session(memory: int) -> mitto.Session:
    """Return a Session whose Machine has `memory` GiB, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    return mitto.Session(machine=mitto.Machine.draw(memory=memory))


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session of a Machine of 8 GiB, on which Chrome runs async scripts of other sites at a low priority."""
    with _session(8) as session:
        yield session


def _run(session: mitto.Session, server: _ScriptServer, name: str) -> tuple[Page, list[str]]:
    """Open case `name`'s page and return the Page and what the page logged, with the server's origin as {origin}."""
    server.case = _CASES[name]
    page = session._open_page(server.url(), timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]
    logged: list[str] = page.evaluate("done", timeout=_TIMEOUT_SECONDS)
    origin = server.url("").rstrip("/")
    return page, [line.replace(origin, "{origin}") for line in logged]


def _console_errors(page: Page, server: _ScriptServer) -> set[str]:
    """Return the console's errors, each once, with the origin of the server and of its other name written short."""
    origin = server.url("").rstrip("/")
    other = origin.replace("127.0.0.1", "localhost")
    return {
        message.text.replace(origin, "{origin}").replace(other, "{other}")
        for message in page.console_messages
        if message.level == "error"
    }


# What Chrome 157.0.8103.51 logged for each case, with the server's origin as {origin}.
_CHROME: dict[str, list[str]] = {
    "parser_order": [
        "i1 loading i1",
        "timer from i1",
        "b1 loading b1",
        "microtask from b1",
        "timer from b1",
        "i2 loading i2",
        "readystatechange interactive",
        "d1 interactive d1",
        "d2 interactive d2",
        "microtask from d2",
        "DOMContentLoaded interactive null",
        "timer from d1",
        "timer from DOMContentLoaded",
        "a1 interactive a1",
        "readystatechange complete",
        "load complete null",
    ],
    "async_order": [
        "inline loading anonymous",
        "a2 loading a2",
        "after a2 loading anonymous",
        "DOMContentLoaded",
        "a1 interactive a1",
        "load",
    ],
    "readiness": [
        "timer from inline",
        "readystatechange interactive",
        "DOMContentLoaded",
        "microtask from DOMContentLoaded",
        "readystatechange complete",
        "load",
        "timer from interactive",
        "timer from DOMContentLoaded",
        "timer from complete",
    ],
    "inserted_order": [
        "p1",
        "x1 async true",
        "x1 interactive x1",
        "after x1 interactive null",
        "async false true",
        "parsed async false true",
        "e2 interactive e2",
        "e2 load",
        "e1 interactive e1",
        "e1 load",
        "o1 interactive o1",
        "o1 load",
        "o2 interactive o2",
        "o2 load",
        "load",
    ],
    "inserted_cases": [
        "a inserted",
        "a ran complete a",
        "a text set",
        "b src set",
        "c ran",
        "e ran e-clone",
        "e ran e",
        "f1 sees f2 true",
        "f2 ran",
        "h ran",
        "n outer n",
        "n inner n-inner",
        "n outer again n",
        "current after null",
        "b ran b",
        "b load",
    ],
    "inserted_moved": [
        "marked",
        "i ran i",
        "i load connected false",
    ],
    "script_types": [
        "no type",
        "empty type",
        "text/javascript",
        "spaced and in capitals",
        "javascript1.5",
        "application/ecmascript",
        "x-javascript",
        "jscript",
        "livescript",
        "language javascript",
        "language JavaScript1.2",
        "language empty",
        "type wins over language",
        "empty type with language vbscript",
        "for window event onload",
        "for window event onload()",
        "event alone",
        "for alone",
        "classic true, module true, importmap true, speculationrules true, webbundle true, text/javascript false,  false, json false, CLASSIC false,  classic false",
    ],
    "load_and_error": [
        "inserted",
        "empty src error null false false",
        "spaces error null false false",
        "invalid URL error null false false",
        "ok ran ok",
        "microtask from ok",
        "ok load null",
        "not found error null false false",
        "server error error null false false",
        "text/html ran",
        "text/html load null",
        "text/plain ran",
        "text/plain load null",
        "text/plain with nosniff error null false false",
        "image/png error null false false",
        "text/csv error null false false",
        "no type ran",
        "no type load null",
        "redirected ran redirected",
        "redirected load null",
        "last ran",
        "last load null",
    ],
    "base_url": [
        "b b",
        "baseURI {origin}/sub/dir/",
        "script src {origin}/sub/dir/b.js",
        "request {origin}/sub/dir/r",
        "base href {origin}/ {origin}/sub/dir/ {origin}/other/",
        "fetch sub",
        "after remove {origin}/other/",
        "invalid about:blank http://[",
        "threw TypeError: Failed to construct 'Request': Failed to parse URL from r",
        "threw TypeError: Failed to construct 'Request': Failed to parse URL from /r",
        "empty {origin}/ {origin}/",
        "none {origin}/ {origin}/",
        "late {origin}/late/ {origin}/late/",
        "l {origin}/late/l.js",
    ],
    "current_script": [
        "inline s1",
        "microtask s1",
        "timer null",
        "external s2",
        "anonymous inline anonymous true",
        "deferred s3",
        "listener null",
        "async s4",
        "load null",
    ],
    "integrity": [
        "parsed-right ran",
        "inline with a wrong digest ran",
        "sha256 ran",
        "sha256 load",
        "sha384 ran",
        "sha384 load",
        "sha512 ran",
        "sha512 load",
        "wrong sha384 error",
        "unknown algorithm ran",
        "unknown algorithm load",
        "empty digest ran",
        "empty digest load",
        "not base64 ran",
        "not base64 load",
        "strongest matches ran",
        "strongest matches load",
        "strongest does not error",
        "one of two of a kind ran",
        "one of two of a kind load",
        "base64url ran",
        "base64url load",
        "with options ran",
        "with options load",
        "in capitals ran",
        "in capitals load",
        "spaces only ran",
        "spaces only load",
        "wrong in capitals ran",
        "wrong in capitals load",
        "wrong base64url error",
        "wrong with options error",
        "wrong without padding error",
        "cross-origin error",
        "cross-origin without integrity ran",
        "cross-origin without integrity load",
        "last ran",
        "last load",
    ],
    "cross_site_async": [
        "chain start",
        "same-site ran while the chain runs true",
        "chain end",
        "cross-site ran while the chain runs false",
    ],
}


# The errors that Chrome's console said for those cases, each once.
_CHROME_CONSOLE: dict[str, list[str]] = {
    "load_and_error": [
        "Refused to execute script from '{origin}/plain-nosniff.js' because its MIME type ('text/plain') is not executable, and strict MIME type checking is enabled.",
        "Refused to execute script from '{origin}/image.js' because its MIME type ('image/png') is not executable.",
        "Refused to execute script from '{origin}/csv.js' because its MIME type ('text/csv') is not executable.",
    ],
    "integrity": [
        "Failed to find a valid digest in the 'integrity' attribute for resource '{origin}/sri.js' with computed SHA-384 integrity '0Gp4OjvCcox88lrZp2Cp+XMcHW3ioQRkH45Dl4m1362p4qgBpP6BOxQHr3SCGEsr'. The resource has been blocked.",
        "Error parsing 'integrity' attribute ('md5-guHnh0cguWMWm+0PtbVBHg=='). The specified hash algorithm must be one of 'sha256', 'sha384', 'sha512', or 'ed21159'.",
        "Error parsing 'integrity' attribute ('sha384-'). The digest must be a valid, base64-encoded value.",
        "Error parsing 'integrity' attribute ('sha384-***'). The digest must be a valid, base64-encoded value.",
        "Ignoring unrecognized 'integrity' attribute option 'foo=bar'.",
        "Error parsing 'integrity' attribute ('SHA384-0Gp4OjvCcox88lrZp2Cp+XMcHW3ioQRkH45Dl4m1362p4qgBpP6BOxQHr3SCGEsr'). The specified hash algorithm must be one of 'sha256', 'sha384', 'sha512', or 'ed21159'.",
        "Error parsing 'integrity' attribute ('SHA384-cxDC6gxVNhsMTeaTsivXLipQo07lvVmMeZGMm5Ps2BLBUyChqccm/+R/uLm9txAr'). The specified hash algorithm must be one of 'sha256', 'sha384', 'sha512', or 'ed21159'.",
        "Subresource Integrity: The resource '{other}/sri.js' has an integrity attribute, but the resource requires the request to be CORS enabled to check the integrity, and it is not. The resource has been blocked because the integrity cannot be enforced.",
        "Failed to find a valid digest in the 'integrity' attribute for resource '{origin}/sri.js' with computed SHA-512 integrity 'Ne7Kjrg8l0i8b0xrwOTVmJYAdRUjJIjyWsYZdBBirLINheUbcX6+Y8juc7qH/AxrkcWu1roAJPieAqM/SkWdhw=='. The resource has been blocked.",
    ],
}


@pytest.mark.parametrize("name", sorted(_CHROME))
def test_scripts_run_in_the_order_chrome_gives(session: mitto.Session, server: _ScriptServer, name: str) -> None:
    page, logged = _run(session, server, name)
    with page:
        assert logged == _CHROME[name]
        assert page.uncaught_errors == []


def test_an_async_script_of_another_site_waits_only_on_a_machine_of_three_gigabytes_or_more(
    native_library: Any, server: _ScriptServer
) -> None:
    # ClassicPendingScript::IsEligibleForLowPriorityAsyncScriptExecution() holds the low priority to a machine of 3 GB
    # or more, so on one of 2 the script runs among the tasks of the normal priority, as the same site\'s does.
    with _session(2) as session:
        page, logged = _run(session, server, "cross_site_async")
        with page:
            assert sorted(line for line in logged if " ran while " in line) == [
                "cross-site ran while the chain runs true",
                "same-site ran while the chain runs true",
            ]


def test_a_script_whose_type_cannot_run_or_whose_digest_fails_says_why_in_the_console(
    session: mitto.Session, server: _ScriptServer
) -> None:
    page, _ = _run(session, server, "load_and_error")
    with page:
        assert _console_errors(page, server) == set(_CHROME_CONSOLE["load_and_error"])
    page, _ = _run(session, server, "integrity")
    with page:
        # Chrome says a message again for each script that reuses a response from its memory cache, and a Page once
        # for each script.
        assert _console_errors(page, server) == set(_CHROME_CONSOLE["integrity"])


def test_a_document_resolves_its_scripts_and_fetches_against_its_base_url(
    session: mitto.Session, server: _ScriptServer
) -> None:
    page, _ = _run(session, server, "base_url")
    with page:
        assert [path for path in server.requests if not path.startswith(("/mark/", "/result"))] == [
            "/",
            "/sub/dir/b.js",
            "/sub/dir/f.json",
            "/late/l.js",
        ]
