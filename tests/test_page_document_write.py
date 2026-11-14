"""Tests of document.write(), writeln(), open() and close() in a Page: markup and scripts that a script writes while the
parser runs, scripts that they hold, which block the parser in turn, what makes a write do nothing or throw, and a write
or an open() that replaces a document.

Each case is a page that logs what runs and what the tree holds, in order, and a local server that holds a response
until the page sends a mark, fetch('/mark/NAME'), so that the order does not hang on the network's timing. The logs are
what Chrome 157.0.8103.51 gave by hand, headless on macOS against the same pages and server. Each test goes through both
APIs, and needs a native library with the Page program beside it, as CI's native job installs it on every Host.
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

_HTML = "text/html; charset=utf-8"

# What every case's page starts with: `log()` and `finish()`, which settles `done` with what was logged, a mark that
# releases what the server holds, the ids in the document, in tree order, and its markup, with the text of its scripts
# left out.
_PRELUDE = (
    "<!doctype html><script>"
    "window.logged = [];"
    "function log(x) { logged.push(String(x)); }"
    "window.done = new Promise(resolve => { window.finish = resolve; });"
    "done.then(l => fetch('/result', {method: 'POST', body: JSON.stringify(l)}));"
    "setTimeout(() => { log('gave up'); finish(logged); }, 5000);"
    "function mark(name) { fetch('/mark/' + name); }"
    "function ids() { return [...document.querySelectorAll('[id]')].map(e => e.id).join(','); }"
    "function tree() { const c = document.documentElement.cloneNode(true);"
    " for (const s of c.querySelectorAll('script')) { s.textContent = ''; } return c.outerHTML; }"
    "</script>"
)


def _page(markup: str) -> Route:
    return Route(_PRELUDE + markup, _HTML)


def _js(source: str, hold: str | None = None) -> Route:
    return Route(source, hold=hold)


_CASES: dict[str, Case] = {}

# Markup and an inline script that a script writes go in just after it, at once, and what that script writes goes in
# just after it in turn.
_CASES["inline_writes"] = Case(
    {
        "/": _page(
            "<body><script id=s1>"
            "log('s1 ' + document.readyState);"
            'document.write(\'<p id=w1>w1</p><script id=s2>log("s2 " + ids());'
            ' document.write("<i id=w3>w3</i>"); log("s2 after " + ids())<\\/script><p id=w2>w2</p>\');'
            "log('s1 after ' + ids());"
            "</script><p id=n>n</p><script>log('end ' + ids()); log(tree()); finish(logged)</script>"
        ),
    }
)

# A script with src that a script writes blocks the parser once the script that wrote it is done: what comes after it,
# written or not, waits for it.
_CASES["written_parser_blocking"] = Case(
    {
        "/": _page(
            "<body><script id=s1>"
            "document.write('<script id=x src=/x.js><\\/script><p id=w1>w1</p>'); log('s1 after write ' + ids());"
            "document.write('<p id=w2>w2</p>'); log('s1 end ' + ids());"
            "setTimeout(() => { log('timer from s1 ' + ids()); mark('t'); });"
            "</script><p id=n>n</p><script>log('n ' + ids()); finish(logged)</script>"
        ),
        "/x.js": _js("log('x ' + document.readyState + ' ' + ids())", hold="t"),
    }
)

# A script with src that the parser waits for writes at the insertion point after it, and what it writes may hold a
# script that blocks the parser again.
_CASES["external_writes"] = Case(
    {
        "/": _page(
            "<body><script id=x src=/x.js></script><p id=after>after</p>"
            "<script>log('end ' + ids()); finish(logged)</script>"
        ),
        "/x.js": _js(
            "document.write('<p id=fromx>fromx</p><script id=y src=/y.js><\\/script><p id=aftery>aftery</p>');"
            "log('x after write ' + ids());"
        ),
        "/y.js": _js("log('y ' + ids()); document.write('<p id=fromy>fromy</p>'); log('y after write ' + ids());"),
    }
)

# Markup written in pieces: a tag, an attribute and a character reference across writes, and an element left open for
# the network's markup to go on in.
_CASES["markup_in_pieces"] = Case(
    {
        "/": _page(
            "<body><script>"
            "document.write('<p id=a'); log('one ' + ids());"
            "document.write('b class=c>x'); log('two ' + ids());"
            "document.write('y</p><p id=e>&am'); document.write('p;</p><b id=bold>bold');"
            "</script> network</b><p id=end>end</p>"
            "<script>log(document.body.innerHTML.replace(/<script>.*?<\\/script>/gs, 'S')); finish(logged)</script>"
        ),
    }
)

# An async or deferred script cannot write into a document that it did not open: the write does nothing, and the
# console says why.
_CASES["async_and_deferred"] = Case(
    {
        "/": _page(
            "<body><script async src=/a.js></script><script defer src=/d.js></script><p id=n>n</p>"
            "<script>"
            "document.addEventListener('DOMContentLoaded', () => { log('DOMContentLoaded ' + ids()); mark('dcl'); });"
            "addEventListener('load', () => { log('load ' + ids()); setTimeout(() => finish(logged), 100); });"
            "</script>"
        ),
        "/a.js": _js("document.write('<p id=froma>'); log('a ' + ids());", hold="dcl"),
        "/d.js": _js("document.write('<p id=fromd>'); log('d ' + ids());"),
    }
)

# A write once the document has loaded opens it anew: its listeners go, it loads again, and close() ends it, with
# DOMContentLoaded and load again for the listeners added since.
_CASES["write_after_load"] = Case(
    {
        "/": _page(
            "<body><p id=old>old</p><script>"
            "document.addEventListener('readystatechange', () => log('old readystatechange ' + document.readyState));"
            "document.addEventListener('DOMContentLoaded', () => log('old DOMContentLoaded'));"
            "addEventListener('load', () => setTimeout(() => {"
            " const html = document.documentElement; const url = document.URL;"
            " log('before ' + document.readyState);"
            ' document.write(\'<p id=new>new</p><script>log("written ran " + document.readyState + " " + ids())<\\/script>\');'
            " log('after write ' + document.readyState + ' ' + (document.documentElement === html) + ' '"
            "  + (document.URL === url) + ' ' + history.length + ' ' + document.compatMode);"
            " log(tree());"
            " document.addEventListener('readystatechange', () => log('readystatechange ' + document.readyState));"
            " document.addEventListener('DOMContentLoaded', () => log('DOMContentLoaded'));"
            " addEventListener('load', () => log('load ' + document.readyState));"
            " document.write('<p id=more>more</p>');"
            " document.close();"
            " log('after close ' + document.readyState + ' ' + ids());"
            " document.close(); log('closed again ' + document.readyState);"
            " setTimeout(() => { log('timer ' + document.readyState); finish(logged); }, 50);"
            "}));"
            "</script>"
        ),
    }
)

# A write from a timer while the parser waits for a script has no insertion point, so it opens the document anew: the
# script that the parser waited for never runs, the rest of the body is dropped, and the document loads only once
# close() is called.
_CASES["write_while_parsing"] = Case(
    {
        "/": _page(
            "<script>"
            "document.addEventListener('DOMContentLoaded', () => log('old DOMContentLoaded'));"
            "setTimeout(() => {"
            " document.write('<p id=new>new</p>'); log('timer ' + document.readyState + ' ' + ids());"
            " addEventListener('load', () => log('load ' + document.readyState));"
            " mark('b');"
            " setTimeout(() => {"
            "  log('later ' + document.readyState + ' ' + ids()); document.close(); log('closed ' + document.readyState);"
            "  setTimeout(() => finish(logged), 100);"
            " }, 300);"
            "});"
            "</script><script id=b src=/b.js></script><p id=rest>rest</p><script>log('rest ran')</script>"
        ),
        "/b.js": _js("log('b ran')", hold="b"),
    }
)

# open() stops a navigation that the page started and that waits for its response, so the page stays, opened.
_CASES["open_stops_navigation"] = Case(
    {
        "/": _page(
            "<body><p id=old>old</p><script>"
            "addEventListener('load', () => setTimeout(() => {"
            " location.href = '/next';"
            " document.open(); document.write('<p id=w>w</p>'); log('wrote ' + ids() + ' ' + document.readyState);"
            " document.close(); log('closed ' + document.readyState);"
            " mark('next'); setTimeout(() => { log('later ' + location.pathname + ' ' + ids()); finish(logged); }, 700);"
            "}));"
            "</script>"
        ),
        "/next": Route("<!doctype html><p id=next>next</p>", _HTML, hold="next"),
    }
)

# open() from a script that the parser runs keeps the document, and so does close() of a document that the network
# loads.
_CASES["open_while_running"] = Case(
    {
        "/": _page(
            "<body><script>"
            "log('open ' + (document.open() === document) + ' ' + document.readyState + ' ' + ids());"
            "document.write('<p id=w>w</p>'); log('wrote ' + ids());"
            "</script><p id=n>n</p><script>"
            "log('n ' + ids()); document.close(); log('close ' + document.readyState + ' ' + ids());"
            "document.open('text/plain', 'replace'); log('open with two ' + ids()); finish(logged);"
            "</script>"
        ),
    }
)

# Writes nested deeper than 21 do nothing, and those within them neither, until the writes have unwound.
_CASES["write_recursion"] = Case(
    {
        "/": _page(
            "<body><script>"
            "window.depth = 0; window.deepest = 0; window.ran = 0;"
            "function nest() { depth++; ran++; deepest = Math.max(deepest, depth);"
            " document.write('<script>nest()<\\/script>'); depth--; }"
            "nest(); log('ran ' + ran + ' deepest ' + deepest);"
            "document.write('<p id=after>after</p>'); log(ids()); finish(logged);"
            "</script>"
        ),
    }
)

# write() takes any number of strings, writeln() adds a line feed, and each value becomes a string.
_CASES["arguments"] = Case(
    {
        "/": _page(
            "<body><script>"
            "document.write('<p id=a>', 'one', 1, null, undefined, {toString() { return 'obj'; }}, '</p>');"
            "document.write(); document.writeln(); document.writeln('<pre id=b>'); document.writeln('x', 'y');"
            "document.write('</pre>');"
            "log(JSON.stringify(document.getElementById('a').textContent));"
            "log(JSON.stringify(document.getElementById('b').textContent));"
            "log(document.write.length + ' ' + document.writeln.length + ' ' + document.open.length + ' '"
            " + document.close.length);"
            "finish(logged);"
            "</script>"
        ),
    }
)

# What throws, and documents without a window, whose scripts do not run.
_CASES["other_documents"] = Case(
    {
        "/": _page(
            "<body><script>"
            "function attempt(name, f) {"
            " try { log(name + ' ' + f()); } catch (e) { log(name + ' threw ' + e.name + ': ' + e.message); } }"
            "const xml = document.implementation.createDocument(null, 'r');"
            "attempt('xml open', () => xml.open());"
            "attempt('xml write', () => xml.write('x'));"
            "attempt('xml writeln', () => xml.writeln('x'));"
            "attempt('xml close', () => xml.close());"
            "attempt('new Document', () => new Document().write('x'));"
            "const d = document.implementation.createHTMLDocument('t');"
            "attempt('html write', () => { d.write('<p id=p>x</p><script>log(\"ran in d\")<\\/script>');"
            " return d.readyState + ' ' + d.body.innerHTML; });"
            "attempt('html write more', () => { d.write('<b>y'); return d.body.innerHTML; });"
            "attempt('html close', () => { d.close(); return d.readyState + ' ' + d.body.innerHTML; });"
            "attempt('html open', () => (d.open() === d) + ' ' + d.readyState + ' ' + d.childNodes.length);"
            "attempt('html three', () => d.open('about:blank', 'x', ''));"
            "attempt('html close again', () => { d.close(); return d.readyState + ' ' + d.documentElement.outerHTML; });"
            "finish(logged);"
            "</script>"
        ),
    }
)

# The whole body has come while the parser waits for a script: once the script has run, the parser takes the rest,
# and readiness changes from a later task, after a timer that the rest set.
_CASES["body_ends_while_waiting"] = Case(
    {
        "/": _page(
            "<script>"
            "document.addEventListener('readystatechange', () => log('readystatechange ' + document.readyState));"
            "document.addEventListener('DOMContentLoaded', () => log('DOMContentLoaded'));"
            "addEventListener('load', () => { log('load'); setTimeout(() => finish(logged), 50); });"
            "setTimeout(() => mark('go'), 200);"
            "</script><script src=/b.js></script>"
            "<script>log('after b ' + document.readyState); setTimeout(() => log('timer from after b'));</script>"
            "<p>end</p>"
        ),
        "/b.js": _js("log('b')", hold="go"),
    }
)

# The errors of a script that a script wrote count from its start, and those of the document's scripts from where they
# are in the document's markup, whatever was written before them.
_CASES["error_positions"] = Case(
    {
        "/": _page(
            "<body><script>"
            "window.onerror = (message, file, line, column) => {"
            " log(message + ' ' + (file === location.href ? 'document' : JSON.stringify(file)) + ' ' + line + ':' + column);"
            " return true; };"
            "document.write('<script>\\n  null.x<\\/script><p>\\n\\n\\n</p>');"
            "</script>\n"
            "<script>\n  null.y</script>\n"
            "<script>finish(logged)</script>"
        ),
    }
)

# Chrome's intervention warns of a script of another site that blocks the parser and that a write inserts, twice in a
# write that leaves the parser waiting, and fetches it, as the desktop's settings block nothing. A script of the same
# site, an async or deferred one and one that the DOM inserts get no warning. The scripts other than the first two run
# in no fixed order, so the case logs only that they ran.
_CASES["intervention"] = Case(
    {
        "/": _page(
            "<body><script>"
            "const other = 'http://localhost:' + location.port;"
            "document.write('<script src=' + other + '/x.js><\\/script>');"
            "document.write('<script src=/same.js><\\/script>');"
            "document.write('<script async src=' + other + '/async.js><\\/script>');"
            "document.write('<script defer src=' + other + '/defer.js><\\/script>');"
            "const inserted = document.createElement('script'); inserted.src = other + '/inserted.js';"
            "document.body.append(inserted);"
            "</script><script>"
            "addEventListener('load', () => { log(ran.sort().join(',')); setTimeout(() => finish(logged), 50); });"
            "</script>"
        ),
        "/x.js": _js("window.ran = ['x'];"),
        "/same.js": _js("ran.push('same');"),
        "/async.js": _js("ran.push('async');"),
        "/defer.js": _js("ran.push('defer');"),
        "/inserted.js": _js("ran.push('inserted');"),
    }
)

# What Chrome 157.0.8103.51 logged for each case, with the server's origin as {origin}.
_CHROME: dict[str, list[str]] = {
    "arguments": [
        '"one1nullundefinedobj"',
        '"xy\\n"',
        "0 0 0 0",
    ],
    "async_and_deferred": [
        "d n",
        "DOMContentLoaded n",
        "a n",
        "load n",
    ],
    "body_ends_while_waiting": [
        "b",
        "after b loading",
        "timer from after b",
        "readystatechange interactive",
        "DOMContentLoaded",
        "readystatechange complete",
        "load",
    ],
    "error_positions": [
        "Uncaught TypeError: Cannot read properties of null (reading 'x') document 2:8",
        "Uncaught TypeError: Cannot read properties of null (reading 'y') document 3:8",
    ],
    "external_writes": [
        "x after write x,fromx,y",
        "y x,fromx,y",
        "y after write x,fromx,y,fromy",
        "end x,fromx,y,fromy,aftery,after",
    ],
    "inline_writes": [
        "s1 loading",
        "s2 s1,w1,s2",
        "s2 after s1,w1,s2,w3",
        "s1 after s1,w1,s2,w3,w2",
        "end s1,w1,s2,w3,w2,n",
        '<html><head><script></script></head><body><script id="s1"></script><p id="w1">w1</p><script id="s2"></script><i id="w3">w3</i><p id="w2">w2</p><p id="n">n</p><script></script></body></html>',
    ],
    "markup_in_pieces": [
        "one ",
        "two ab",
        'S<p id="ab" class="c">xy</p><p id="e">&amp;</p><b id="bold">bold network</b><p id="end">end</p>S',
    ],
    "open_stops_navigation": [
        "wrote w loading",
        "closed complete",
        "later / w",
    ],
    "open_while_running": [
        "open true loading ",
        "wrote w",
        "n w,n",
        "close loading w,n",
        "open with two w,n",
    ],
    "other_documents": [
        "xml open threw InvalidStateError: Failed to execute 'open' on 'Document': Only HTML documents support open().",
        "xml write threw InvalidStateError: Failed to execute 'write' on 'Document': Only HTML documents support write().",
        "xml writeln threw InvalidStateError: Failed to execute 'writeln' on 'Document': Only HTML documents support write().",
        "xml close threw InvalidStateError: Failed to execute 'close' on 'Document': Only HTML documents support close().",
        "new Document threw InvalidStateError: Failed to execute 'write' on 'Document': Only HTML documents support write().",
        'html write loading <p id="p">x</p><script>log("ran in d")</script>',
        'html write more <p id="p">x</p><script>log("ran in d")</script><b>y</b>',
        'html close complete <p id="p">x</p><script>log("ran in d")</script><b>y</b>',
        "html open true loading 0",
        "html three threw InvalidAccessError: Failed to execute 'open' on 'Document': The document has no window associated.",
        "html close again complete <html><head></head><body></body></html>",
    ],
    "write_after_load": [
        "old readystatechange interactive",
        "old DOMContentLoaded",
        "old readystatechange complete",
        "before complete",
        "written ran loading new",
        "after write loading false true 1 BackCompat",
        '<html><head></head><body><p id="new">new</p><script></script></body></html>',
        "readystatechange interactive",
        "DOMContentLoaded",
        "readystatechange complete",
        "load complete",
        "after close complete new,more",
        "closed again complete",
        "timer complete",
    ],
    "write_recursion": [
        "ran 22 deepest 22",
        "after",
    ],
    "write_while_parsing": [
        "timer loading new",
        "later loading new",
        "load complete",
        "closed complete",
    ],
    "written_parser_blocking": [
        "s1 after write s1,x",
        "s1 end s1,x",
        "timer from s1 s1,x",
        "x loading s1,x",
        "n s1,x,w1,w2,n",
    ],
}

# The warnings that Chrome's console said for those cases.
_INTERVENTION = (
    "A parser-blocking, cross site (i.e. different eTLD+1) script, {other}/x.js, is invoked via document.write. The"
    " network request for this script MAY be blocked by the browser in this or a future page load due to poor network"
    " connectivity. If blocked in this page load, it will be confirmed in a subsequent console message. See"
    " https://www.chromestatus.com/feature/5718547946799104 for more details."
)
_CHROME_CONSOLE: dict[str, list[str]] = {
    "intervention": [_INTERVENTION, _INTERVENTION],
    "async_and_deferred": [
        "Failed to execute 'write' on 'Document': It isn't possible to write into a document from an asynchronously-loaded external script unless it is explicitly opened.",
        "Failed to execute 'write' on 'Document': It isn't possible to write into a document from an asynchronously-loaded external script unless it is explicitly opened.",
    ],
}


@pytest.fixture
def server() -> Iterator[ScriptServer]:
    with ScriptServer() as script_server:
        yield script_server


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _run(session: mitto.Session, server: ScriptServer, name: str) -> tuple[Page, list[str]]:
    """Open case `name`'s page and return the Page and what the page logged, with the server's origin as {origin}."""
    server.case = _CASES[name]
    page = session._open_page(server.url(), timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]
    logged: list[str] = page.evaluate("done", timeout=_TIMEOUT_SECONDS)
    origin = server.url("").rstrip("/")
    return page, [line.replace(origin, "{origin}") for line in logged]


@pytest.mark.parametrize("name", sorted(_CHROME))
def test_what_scripts_write_runs_as_chrome_runs_it(session: mitto.Session, server: ScriptServer, name: str) -> None:
    page, logged = _run(session, server, name)
    with page:
        assert logged == _CHROME[name]
        assert page.uncaught_errors == []


def _console_warnings(page: Page, server: ScriptServer) -> list[str]:
    """Return the console's warnings, with the server's other origin, at localhost, as {other}."""
    other = server.url("").rstrip("/").replace("127.0.0.1", "localhost")
    return [message.text.replace(other, "{other}") for message in page.console_messages if message.level == "warning"]


def test_a_script_that_cannot_write_says_why_in_the_console(session: mitto.Session, server: ScriptServer) -> None:
    page, _ = _run(session, server, "async_and_deferred")
    with page:
        assert _console_warnings(page, server) == _CHROME_CONSOLE["async_and_deferred"]


def test_a_script_of_another_site_that_a_write_inserts_is_warned_of_and_fetched(
    session: mitto.Session, server: ScriptServer
) -> None:
    page, logged = _run(session, server, "intervention")
    with page:
        assert logged == ["async,defer,inserted,same,x"]
        assert _console_warnings(page, server) == _CHROME_CONSOLE["intervention"]
        assert page.uncaught_errors == []
