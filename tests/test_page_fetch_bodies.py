"""Tests of FormData, pair iterators and the bodies of forms in the Page program: docs/runtime.md, "fetch()".

Headers, URLSearchParams and FormData iterate as Blink's pair iterables do, FormData keeps Blink's entries, a FormData or
a URLSearchParams body is written as Blink writes it, and formData() parses multipart/form-data and
application/x-www-form-urlencoded bodies as Blink's FetchDataLoaderAsFormData and URLSearchParams do. Each expected
value is what Chrome 157.0.8103.79 gave for the same script, in a page of its own. These run where MITTO_PAGE_PROGRAM
names the Page program, as tests/test_page_program.py does.
"""

import http.server
import os
import subprocess
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 120

# Helpers that the scripts share: a log of what happened in order, a read of a whole stream, a promise's outcome, a
# task's wait, a body's text with its boundary as "B", and a FormData's entries as JSON.
PRELUDE = """
const log = [];
const readAll = async (stream) => {
    const reader = stream.getReader();
    const chunks = [];
    for (;;) {
        const { value, done } = await reader.read();
        if (done) return chunks;
        chunks.push(value);
    }
};
const settle = (promise) => promise.then((v) => "fulfilled " + String(v), (e) => "rejected " + String(e));
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
const bodyOf = async (body) => {
    const r = new Request("https://example.test/", { method: "POST", body });
    const type = r.headers.get("content-type");
    const boundary = type.split("boundary=")[1];
    const text = await r.text();
    return type.replace(boundary, "B") + " / " + JSON.stringify(text.split(boundary).join("B"));
};
const entries = async (formData) => {
    const out = [];
    for (const [name, value] of formData) {
        out.push(typeof value === "string" ? [name, value]
            : [name, value.constructor.name, value.name, value.type, value.size, await value.text()]);
    }
    return JSON.stringify(out);
};
const parse = (body, type) => new Response(body, type === undefined ? {} : { headers: { "content-type": type } })
    .formData().then(entries, (e) => "rejected " + e);
"""

# Each script and what Chrome 157.0.8103.79 gave for it, as _evaluate() writes it.
BODIES = {
    "a pair iterator's results": (
        "const it = new Headers([['b', '2'], ['a', '1']]).entries();"
        " return JSON.stringify([it.next(), it.next(), it.next()])",
        'value: [{"done":false,"value":["a","1"]},{"done":false,"value":["b","2"]},{"done":true}]',
    ),
    "a setlike's and a maplike's iterator results": (
        "CSS.highlights.set('x', new Highlight());"
        " return [document.fonts.values().next(), CSS.highlights.entries().next()]"
        ".map((r) => Object.keys(r).join('+')).join()",
        "value: done+value,done+value",
    ),
    "a pair iterator's prototype": (
        "const h = new Headers(); const it = h.keys(); const proto = Object.getPrototypeOf(it);"
        " const iteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));"
        " return [Object.prototype.toString.call(it), Object.getPrototypeOf(proto) === iteratorPrototype,"
        " h[Symbol.iterator] === h.entries, Object.getOwnPropertyNames(proto).join('+'), proto.next.length,"
        " Object.getPrototypeOf(new URLSearchParams().keys()) === proto].join()",
        "value: [object Headers Iterator],true,true,next,0,false",
    ),
    "next() of another object": (
        "const next = new Headers().keys().next; return next.call(new URLSearchParams().keys())",
        "threw: TypeError: Illegal invocation",
    ),
    "headers sorted and combined": (
        "const h = new Headers([['B', '1'], ['a', '2'], ['b', '3'], ['Set-Cookie', 'x'], ['set-cookie', 'y'],"
        " ['C', '']]); return JSON.stringify([...h]) + ' / ' + [...h.keys()].join() + ' / ' + [...h.values()].join()",
        'value: [["a","2"],["b","1, 3"],["c",""],["set-cookie","x"],["set-cookie","y"]] / a,b,c,set-cookie,set-cookie / 2,1, 3,,x,y',
    ),
    "headers that change while they iterate": (
        "const h = new Headers([['a', '1'], ['c', '3']]); const it = h.keys(); const first = it.next().value;"
        " h.append('b', '2'); const rest = [...it]; h.append('d', '4');"
        " return [first, rest.join('+'), JSON.stringify(it.next())].join(' / ')",
        'value: a / b+c / {"done":false,"value":"d"}',
    ),
    "a pair iterator with nothing left takes what comes": (
        "const p = new URLSearchParams(); const it = p.keys(); const before = JSON.stringify(it.next());"
        " p.append('x', '1'); return before + ' / ' + JSON.stringify(it.next())",
        'value: {"done":true} / {"done":false,"value":"x"}',
    ),
    "search params that change while they iterate": (
        "const p = new URLSearchParams('a=1&b=2&c=3'); const out = [];"
        " for (const [k, v] of p) { out.push(k + v); if (k === 'a') p.delete('a'); }"
        " return out.join() + ' / ' + [...new URLSearchParams('x=1&y=2&x=3').entries()].join('|')",
        "value: a1,c3 / x,1|y,2|x,3",
    ),
    "forEach()": (
        "const h = new Headers([['a', '1'], ['b', '2']]); const out = [];"
        " h.forEach(function (v, k, o) { out.push([v, k, o === h, this.x, arguments.length].join(':')); }, { x: 7 });"
        " const p = new URLSearchParams('q=1'); p.forEach(function (v, k, o) { out.push([v, k, o === p, this === undefined].join(':')); });"
        " return out.join(' ') + ' / ' + h.forEach.length",
        "value: 1:a:true:7:3 2:b:true:7:3 1:q:true:false / 1",
    ),
    "forEach() of something that is not a function": (
        "return [new Headers(), new URLSearchParams(), new FormData()].map((o) => { try { o.forEach(1); }"
        " catch (e) { return e.name + ': ' + e.message; } }).join(' / ')",
        "value: TypeError: Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'Function'. / TypeError: Failed to execute 'forEach' on 'URLSearchParams': parameter 1 is not of type 'Function'. / TypeError: Failed to execute 'forEach' on 'FormData': parameter 1 is not of type 'Function'.",
    ),
    "a forEach() callback that throws": (
        "const p = new URLSearchParams('a=1&b=2'); const seen = [];"
        " try { p.forEach((v, k) => { seen.push(k); throw new Error('stop'); }); } catch (e) { seen.push(e.message); }"
        " return seen.join()",
        "value: a,stop",
    ),
    "a FormData's entries": (
        "const f = new FormData(); f.append('a', '1'); f.append('b', '2'); f.append('a', '3');"
        " const before = [f.get('a'), f.getAll('a').join('+'), f.has('b'), f.get('z'), f.getAll('z').length].join();"
        " f.set('a', '4'); f.append('c', '5'); f.delete('b');"
        " return before + ' / ' + JSON.stringify([...f]) + ' / ' + [...f.keys()].join() + ' / ' + [...f.values()].join()",
        'value: 1,1+3,true,,0 / [["a","4"],["c","5"]] / a,c / 4,5',
    ),
    "set() keeps the first place": (
        "const f = new FormData(); f.append('x', '1'); f.append('y', '2'); f.append('x', '3'); f.set('x', '4');"
        " f.set('z', '5'); return JSON.stringify([...f])",
        'value: [["x","4"],["y","2"],["z","5"]]',
    ),
    "a Blob in a FormData": (
        "const f = new FormData(); const b = new Blob(['xy'], { type: 'Text/Plain' }); const before = Date.now();"
        " f.append('b', b); f.append('n', b, 'named.txt'); const v = f.get('b'); const n = f.get('n');"
        " return [v instanceof File, v === b, v.name, v.type, v.size, v.lastModified >= before, await v.text(),"
        " n.name, typeof f.getAll('b')[0]].join()",
        "value: true,false,blob,text/plain,2,true,xy,named.txt,object",
    ),
    "a File in a FormData": (
        "const f = new FormData(); const file = new File(['abc'], 'orig.txt', { type: 'a/b', lastModified: 5 });"
        " f.append('same', file); f.append('renamed', file, 'new.txt'); f.set('empty', file, '');"
        " const r = f.get('renamed');"
        " return [f.get('same') === file, r === file, r.name, r.lastModified, r.type, f.get('empty').name === ''].join()",
        "value: true,false,new.txt,5,a/b,true",
    ),
    "what append() and set() take": (
        "const f = new FormData(); f.append('n', null); f.append('u', undefined); f.append('o', { toString() { return 'O'; } });"
        " f.append(1, 2); f.set('\\ud800a', 'b\\udc00');"
        " const errors = [() => f.append('a', 'b', 'c'), () => f.set('a', 'b', 'c'), () => f.append('a')].map((t) => {"
        " try { t(); return 'ok'; } catch (e) { return e.name + ': ' + e.message; } });"
        " return JSON.stringify([...f]) + ' / ' + errors.join(' / ')",
        'value: [["n","null"],["u","undefined"],["o","O"],["1","2"],["�a","b�"]] / TypeError: Failed to execute \'append\' on \'FormData\': parameter 2 is not of type \'Blob\'. / TypeError: Failed to execute \'set\' on \'FormData\': parameter 2 is not of type \'Blob\'. / TypeError: Failed to execute \'append\' on \'FormData\': 2 arguments required, but only 1 present.',
    ),
    "the FormData constructor": (
        "const errors = [() => new FormData(document.body), () => new FormData(null), () => FormData()].map((t) => {"
        " try { t(); return 'ok'; } catch (e) { return e.name + ': ' + e.message; } });"
        " return [FormData.length, new FormData(undefined) instanceof FormData, ...errors].join(' / ')",
        "value: 0 / true / TypeError: Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'. / TypeError: Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'. / TypeError: Failed to construct 'FormData': Please use the 'new' operator, this DOM object constructor cannot be called as a function.",
    ),
    "a FormData body": (
        "const f = new FormData(); f.append('a', 'x\\ny\\rz\\r\\n'); f.append('q\"\\r\\n\\n', 'v'); f.append('\\u00e9', '\\u20ac');"
        " return bodyOf(f)",
        'value: multipart/form-data; boundary=B / "--B\\r\\nContent-Disposition: form-data; name=\\"a\\"\\r\\n\\r\\nx\\r\\ny\\r\\nz\\r\\n\\r\\n--B\\r\\nContent-Disposition: form-data; name=\\"q%22%0D%0A%0D%0A\\"\\r\\n\\r\\nv\\r\\n--B\\r\\nContent-Disposition: form-data; name=\\"é\\"\\r\\n\\r\\n€\\r\\n--B--\\r\\n"',
    ),
    "a FormData body's boundary": (
        "const f = new FormData(); const types = [];"
        " for (let i = 0; i < 20; ++i) types.push(new Request('https://example.test/', { method: 'POST', body: f })"
        " .headers.get('content-type'));"
        " return [new Set(types).size, types.every((t) => /^multipart\\/form-data; boundary=----WebKitFormBoundary"
        "[A-Za-z0-9]{16}$/.test(t)), JSON.stringify(await new Response(f).text()).replace(/WebKitFormBoundary[A-Za-z0-9]{16}/, 'R')].join(' / ')",
        'value: 20 / true / "------R--\\r\\n"',
    ),
    "a FormData body with files": (
        "const f = new FormData(); f.append('t', new File(['abc'], 'a.txt', { type: 'Text/Plain;Charset=x' }));"
        " f.append('u', new Blob(['\\u00ff'])); f.append('q', new File([''], 'q\"\\r\\n\\u00e9.bin'));"
        " f.append('r', new Blob(['z']), 'r\\n.txt'); return bodyOf(f)",
        'value: multipart/form-data; boundary=B / "--B\\r\\nContent-Disposition: form-data; name=\\"t\\"; filename=\\"a.txt\\"\\r\\nContent-Type: text/plain;charset=x\\r\\n\\r\\nabc\\r\\n--B\\r\\nContent-Disposition: form-data; name=\\"u\\"; filename=\\"blob\\"\\r\\nContent-Type: application/octet-stream\\r\\n\\r\\nÿ\\r\\n--B\\r\\nContent-Disposition: form-data; name=\\"q\\"; filename=\\"q%22%0D%0Aé.bin\\"\\r\\nContent-Type: application/octet-stream\\r\\n\\r\\n\\r\\n--B\\r\\nContent-Disposition: form-data; name=\\"r\\"; filename=\\"r%0A.txt\\"\\r\\nContent-Type: application/octet-stream\\r\\n\\r\\nz\\r\\n--B--\\r\\n"',
    ),
    "a URLSearchParams body": (
        "const p = new URLSearchParams([['a b', 'c&d'], ['\\u00e9', '*-._~!']]);"
        " const r = new Request('https://example.test/', { method: 'POST', body: p });"
        " return [r.headers.get('content-type'), await r.text(),"
        " new Response(new URLSearchParams('x=1')).headers.get('content-type')].join(' / ')",
        "value: application/x-www-form-urlencoded;charset=UTF-8 / a+b=c%26d&%C3%A9=*-._%7E%21 / application/x-www-form-urlencoded;charset=UTF-8",
    ),
    "a body's Content-Type set beside it": (
        "const f = new FormData(); f.append('a', '1');"
        " const r = new Response(f, { headers: { 'content-type': 'text/x' } });"
        " return [r.headers.get('content-type'), (await r.text()).startsWith('------WebKitFormBoundary')].join()",
        "value: text/x,true",
    ),
    "when a FormData body's read settles": (
        "const simple = new FormData(); simple.append('a', '1'); const withFile = new FormData();"
        " withFile.append('f', new Blob(['x']));"
        " for (const [name, f] of [['simple', simple], ['file', withFile]]) {"
        " const reader = new Response(f).body.getReader(); setTimeout(() => log.push(name + ' timeout'), 0);"
        " const { value } = await reader.read(); log.push(name + ' read ' + value.length); await tick(); }"
        " return log.join()",
        "value: simple read 133,simple timeout,file timeout,file read 190",
    ),
    "formData() of a FormData body": (
        "const f = new FormData(); f.append('a', '1\\r\\n2'); f.append('f', new File(['zz'], 'z\"\\n.txt', { type: 'Text/X' }));"
        " f.append('b', new Blob(['q'])); f.append('\\u00e9', '\\u20ac');"
        " const g = await new Response(f).formData(); const file = g.get('f');"
        " return [await entries(g), file.lastModified >= Date.now() - 1000, g.get('f') === file].join(' / ')",
        'value: [["a","1\\r\\n2"],["f","File","z%22%0A.txt","text/x",2,"zz"],["b","File","blob","application/octet-stream",1,"q"],["é","€"]] / true / true',
    ),
    "formData() of a request": (
        "const f = new FormData(); f.append('k', 'v');"
        " const r = new Request('https://example.test/', { method: 'POST', body: f });"
        " const g = await r.formData(); return [await entries(g), r.bodyUsed].join(' / ')",
        'value: [["k","v"]] / true',
    ),
    "formData() of an urlencoded body": (
        "return [await parse('a=1&b=%20+x&&c&=d&%zz=%e9%', 'application/x-www-form-urlencoded'),"
        " await parse(new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0x3d, 0xff]), 'Application/X-WWW-Form-Urlencoded; charset=x'),"
        " await parse('?q=1', 'application/x-www-form-urlencoded'),"
        " await parse('a&b=1&c&d==2&=&e', 'application/x-www-form-urlencoded')].join(' / ')",
        'value: [["a","1"],["b","  x"],["c",""],["","d"],["%zz","�%"]] / [["﻿a","�"]] / [["?q","1"]]'
        ' / [["a",""],["b","1"],["c",""],["d","=2"],["",""],["e",""]]',
    ),
    "formData() of a body of another type": (
        "const r = new Response('a=1'); const outcome = await settle(r.formData());"
        " return [outcome, r.bodyUsed, await parse('a=1', 'text/plain'), await parse('a=1', 'multipart/mixed; boundary=x'),"
        " await parse('a=1', 'application/x-www-form-urlencoded-x')].join(' / ')",
        "value: rejected TypeError: Failed to fetch / true / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch",
    ),
    "formData() of a null body": (
        "return [await parse(null), await parse(null, 'application/x-www-form-urlencoded'),"
        " await parse(null, 'multipart/form-data; boundary=x'), await parse(null, 'text/plain')].join(' / ')",
        "value: rejected TypeError: Invalid MIME type / [] / rejected TypeError: Invalid MIME type / rejected TypeError: Invalid MIME type",
    ),
    "formData() of multipart without a boundary": (
        "const r = new Response('x', { headers: { 'content-type': 'multipart/form-data' } });"
        " const outcome = await settle(r.formData()); return [outcome, r.bodyUsed,"
        " await parse('x', 'multipart/form-data; boundary='), await parse('x', 'multipart/form-data; boundary=\"\"'),"
        " await parse('x', 'multipart/form-data; boundary')].join(' / ')",
        "value: rejected TypeError: Invalid MIME type / false / rejected TypeError: Invalid MIME type / rejected TypeError: Invalid MIME type / rejected TypeError: Invalid MIME type",
    ),
    "formData() of multipart bodies": (
        "const ok = (body) => parse(body, 'Multipart/Form-Data; Boundary=\"b b\"');"
        " return [await ok('preamble\\r\\n--b b\\r\\nContent-Disposition: form-data; name=\"a\"\\r\\n\\r\\n1\\r\\n--b b--\\r\\nepilogue'),"
        ' await ok(\'--b b \\t\\r\\ncontent-disposition:form-data;name=x;filename="f\\\\"g"\\r\\n\\r\\nbytes\\r\\n--b b-- \\r\\n\'),'
        ' await ok(\'--b b\\r\\nContent-Disposition: form-data; name="t"; filename=""\\r\\nContent-Type: A/B; q=1\\r\\n\\r\\n\\r\\n--b b--\'),'
        " await ok('--b b\\r\\nContent-Disposition: form-data;\\r\\n name=\"folded\"\\r\\n\\r\\nv\\r\\n--b b--'),"
        " await ok('--b b\\r\\nContent-Disposition: form-data; name=\"u\"\\r\\n\\r\\n\\u00e9\\ufeff\\r\\n--b b--\\r\\n')].join(' / ')",
        'value: [["a","1"]] / [["x","File","f\\"g","text/plain",5,"bytes"]] / [["t","File","","A/B; q=1",0,""]] / [["folded","v"]] / [["u","é﻿"]]',
    ),
    "formData() of multipart bodies that do not parse": (
        "const bad = (body) => parse(body, 'multipart/form-data; boundary=b');"
        " return [await bad('--b\\r\\nContent-Disposition: form-data\\r\\n\\r\\nv\\r\\n--b--'),"
        " await bad('--b\\r\\nContent-Disposition: Form-Data; name=a\\r\\n\\r\\nv\\r\\n--b--'),"
        " await bad('--b\\r\\nContent-Disposition: form-data; name=a\\r\\n\\r\\nv\\r\\n--b'),"
        " await bad('--b\\r\\nContent-Disposition: form-data; name=a\\r\\n\\r\\nv'),"
        " await bad('--b\\r\\nContent-Disposition: form-data; name=a\\r\\nContent-Disposition: form-data; name=b\\r\\n\\r\\nv\\r\\n--b--'),"
        " await bad(new Uint8Array([...new TextEncoder().encode('--b\\r\\nContent-Disposition: form-data; name=a\\r\\n\\r\\n'),"
        " 0xff, ...new TextEncoder().encode('\\r\\n--b--')])),"
        " await bad('--b\\r\\n\\r\\nv\\r\\n--b--'), await bad('--bx\\r\\n'), await bad('')].join(' / ')",
        'value: rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / [["a","v"]] / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch / rejected TypeError: Failed to fetch',
    ),
    "formData() of a file part without a type": (
        "const g = await new Response('--b\\r\\nContent-Disposition: form-data; name=f; filename=x\\r\\n\\r\\nabc\\r\\n--b--',"
        " { headers: { 'content-type': 'multipart/form-data; boundary=b' } }).formData(); const f = g.get('f');"
        " return [f.type, f.name, f.size, f.lastModified >= Date.now() - 1000].join()",
        "value: text/plain,x,3,true",
    ),
    "formData() of a body that streams": (
        "const parts = ['--b\\r\\nContent-Disposition: form-data; name=a\\r\\n\\r\\n1', '2\\r\\n--', 'b--'];"
        " const stream = new ReadableStream({ pull(c) { const p = parts.shift(); if (p === undefined) c.close();"
        " else c.enqueue(new TextEncoder().encode(p)); } });"
        " return parse(stream, 'multipart/form-data; boundary=b')",
        'value: [["a","12"]]',
    ),
    "formData() of a body in pieces": (
        "const bytes = new TextEncoder().encode('--b\\r\\nContent-Disposition: form-data; name=a\\r\\nX: y\\r\\n\\r\\n1\\r\\n--b"
        "\\r\\nContent-Disposition: form-data; name=c; filename=d\\r\\n\\r\\n2\\n\\n3\\r\\n\\r\\n--b--');"
        " const run = (pieces) => parse(new ReadableStream({ pull(c) { const p = pieces.shift(); if (p === undefined) c.close();"
        " else c.enqueue(p); } }), 'multipart/form-data; boundary=b'); const seen = new Set();"
        " for (let i = 1; i < bytes.length; ++i) seen.add(await run([bytes.slice(0, i), bytes.slice(i)]));"
        " seen.add(await run([...bytes].map((b) => Uint8Array.of(b)))); return [...seen].join(' / ')",
        'value: [["a","1"],["c","File","d","text/plain",6,"2\\n\\n3\\r\\n"]]',
    ),
    "a body whose conversion throws": (
        "const value = { toString() { throw new Error('mine'); } };"
        " const proxy = new Proxy({}, { get() { throw new Error('proxy'); } });"
        " const tried = (make) => { try { return 'made ' + make(); } catch (e) { return 'threw ' + e; } };"
        " return [value, proxy, [value], Symbol()].map((body) => tried(() => new Response(body)) + ' / '"
        " + tried(() => new Request('https://a.test/', { method: 'POST', body }))).join(' / ')",
        "value: threw Error: mine / threw Error: mine / threw Error: proxy / threw Error: proxy / threw Error: mine"
        " / threw Error: mine / threw TypeError: Failed to construct 'Response': Cannot convert a Symbol value to a string"
        " / threw TypeError: Failed to construct 'Request': Cannot convert a Symbol value to a string",
    ),
    "formData() after the body is used": (
        "const r = new Response('a=1', { headers: { 'content-type': 'application/x-www-form-urlencoded' } });"
        " await r.text(); try { return await settle(r.formData()); } catch (e) { return 'threw ' + e; }",
        "value: rejected TypeError: Failed to execute 'formData' on 'Response': body stream already read",
    ),
}


@pytest.fixture(scope="module")
def program() -> Path:
    value = os.environ.get(_PROGRAM_ENV_VAR)
    if not value:
        pytest.skip(f"{_PROGRAM_ENV_VAR} does not name the Page program")
    return Path(value)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_streams.py writes it.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {PRELUDE} {script} }})();
            return "value: " + (typeof value === "object" && value !== null
                ? Object.prototype.toString.call(value) : String(value));
        }} catch (error) {{
            return "threw: " + error;
        }}
    }})()"""
    result = subprocess.run(
        [program, "-"], input=wrapped, capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


@pytest.mark.parametrize(("script", "chrome"), BODIES.values(), ids=BODIES.keys())
def test_form_bodies_and_iterators_do_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome


@pytest.fixture
def page_url(native_library: Any) -> Iterator[str]:
    """The URL of an empty page on 127.0.0.1, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            body = b"<!doctype html><title>t</title><body>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


def test_a_pair_iterator_and_its_results_are_of_the_receivers_realm(page_url: str) -> None:
    # As Chrome 157 gave it: a frame's entries() called on the window's Headers gives the window's iterator, and the
    # frame's next() called on that iterator gives a result of the window's realm, whose pair is of the frame's.
    script = """
      (() => {
        const f = document.createElement('iframe'); document.body.append(f); const w = f.contentWindow;
        const h = new Headers([['a', '1']]);
        const it = w.Headers.prototype.entries.call(h);
        const own = Object.getPrototypeOf(h.entries());
        const theirs = Object.getPrototypeOf(new w.Headers().entries());
        const step = theirs.next.call(it);
        return [Object.getPrototypeOf(it) === own, Object.getPrototypeOf(it) === theirs,
                Object.getPrototypeOf(step) === Object.prototype, Object.getPrototypeOf(step) === w.Object.prototype,
                Object.getPrototypeOf(step.value) === Array.prototype,
                Object.getPrototypeOf(step.value) === w.Array.prototype].join();
      })()
    """
    with (
        mitto.Session() as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == "true,false,true,false,false,true"


def test_form_datas_get_all_gives_an_array_of_the_receivers_realm(page_url: str) -> None:
    # As Chrome 157 gave it: a frame's getAll() called on the window's FormData gives an array and Files of the window.
    script = """
      (() => {
        const f = document.createElement('iframe'); document.body.append(f); const w = f.contentWindow;
        const fd = new FormData(); fd.append('a', new Blob(['x']));
        const all = w.FormData.prototype.getAll.call(fd, 'a');
        return [Object.getPrototypeOf(all) === Array.prototype, Object.getPrototypeOf(all) === w.Array.prototype,
                Object.getPrototypeOf(all[0]) === File.prototype,
                Object.getPrototypeOf(all[0]) === w.File.prototype].join();
      })()
    """
    with (
        mitto.Session() as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == "true,false,true,false"


def test_iterators_and_form_members_refuse_another_window_as_chrome_does(page_url: str) -> None:
    # As Chrome 157 gave it: a member called on the window of a frame of another origin throws, one that returns a promise
    # too, and on one of its own origin, or whose frame has gone, throws a TypeError, or rejects with it. The first throw
    # is Chrome's SecurityError, where the Page throws V8's own TypeError so far (docs/runtime.md, "What the bindings run
    # on").
    script = """
      (async () => {
        const iteratorOf = (c) => Object.getPrototypeOf(new c().entries());
        const calls = [
          (w) => Headers.prototype.entries.call(w), (w) => Headers.prototype.forEach.call(w, () => 1),
          (w) => Headers.prototype[Symbol.iterator].call(w), (w) => iteratorOf(Headers).next.call(w),
          (w) => URLSearchParams.prototype.values.call(w), (w) => iteratorOf(URLSearchParams).next.call(w),
          (w) => FormData.prototype.append.call(w, 'a', 'b'), (w) => FormData.prototype.getAll.call(w, 'a'),
          (w) => FormData.prototype.keys.call(w), (w) => iteratorOf(FormData).next.call(w),
          (w) => Response.prototype.formData.call(w), (w) => Request.prototype.formData.call(w),
        ];
        const outcome = (call, w) => {
          let result;
          try { result = call(w); } catch (e) { return 'threw'; }
          return result instanceof Promise ? result.then(() => 'resolved', () => 'rejected') : 'returned';
        };
        const outcomes = async (w) => (await Promise.all(calls.map((call) => outcome(call, w)))).join(' ');
        const frame = () => { const f = document.createElement('iframe'); document.body.append(f); return f; };
        const own = frame();
        const other = frame();
        await new Promise((resolve) => { other.onload = resolve; other.src = 'data:text/html,x'; });
        const gone = frame();
        const goneWindow = gone.contentWindow;
        gone.remove();
        return [await outcomes(other.contentWindow), await outcomes(own.contentWindow), await outcomes(goneWindow)];
      })()
    """
    with (
        mitto.Session() as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == [
            " ".join(["threw"] * 12),
            " ".join(["threw"] * 10 + ["rejected"] * 2),
            " ".join(["threw"] * 10 + ["rejected"] * 2),
        ]


@pytest.mark.parametrize(
    "leave", ["f.remove();", "await new Promise((r) => { f.onload = r; f.src = 'data:text/html,x'; });"]
)
def test_what_a_frames_old_window_made_still_iterates_once_the_frame_has_left_it(page_url: str, leave: str) -> None:
    # As Chrome 157 gave it, for a frame that has gone and one that shows another origin's document.
    script = """
      (async () => {
        const f = document.createElement('iframe'); document.body.append(f); const w = f.contentWindow;
        const [H, P, F] = [w.Headers, w.URLSearchParams, w.FormData];
        const headers = new H([['a', '1'], ['b', '2']]); const iterator = headers.entries(); iterator.next();
        const params = new P('a=1&b=2'); const keys = params.keys();
        const form = new F(); form.append('a', '1'); const values = form.values();
        LEAVE
        const own = new Headers([['c', '3']]);
        const pairs = (list) => { const all = []; list.forEach((value, key) => all.push(key + value)); return all.join(); };
        const theirNext = Object.getPrototypeOf(headers.entries()).next;
        return JSON.stringify([
          iterator.next(), keys.next(), values.next(), [...headers], pairs(headers), pairs(params), pairs(form),
          [...Headers.prototype.entries.call(headers)], [...H.prototype.entries.call(own)], theirNext.call(own.entries()),
          [...new H([['z', '9']])], form.getAll('a'),
        ]);
      })()
    """.replace("LEAVE", leave)
    expected = (
        '[{"done":false,"value":["b","2"]},{"done":false,"value":"a"},{"done":false,"value":"1"},'
        '[["a","1"],["b","2"]],"a1,b2","a1,b2","a1",'
        '[["a","1"],["b","2"]],[["c","3"]],{"done":false,"value":["c","3"]},[["z","9"]],["1"]]'
    )
    with (
        mitto.Session() as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == expected


# Helpers of the realm scripts below: a frame, which realm a value is of ("the window's/the frame's"), the outcome of a
# promise or "pending", and a body of each read with the init that gives it its type.
_REALM_PRELUDE = """
const frame = () => { const f = document.createElement("iframe"); document.body.append(f); return f; };
const of = (v, w) => v === null || typeof v !== "object" ? typeof v : (v instanceof Object) + "/" + (v instanceof w.Object);
const promiseOf = (p, w) => (p instanceof Promise) + "/" + (p instanceof w.Promise);
const outcome = (p) => Promise.race([
    p.then((v) => "ok " + (v && v.get ? v.get("a") : v), (e) => "rejected " + e.name + ": " + e.message),
    new Promise((resolve) => setTimeout(() => resolve("pending"), 300))]);
const URL_ENCODED = { headers: { "content-type": "application/x-www-form-urlencoded" } };
const MULTIPART = { headers: { "content-type": "multipart/form-data; boundary=x" } };
const BODIES = [
    ["text", "x"], ["json", '{"a":1}'], ["arrayBuffer", "x"], ["blob", "x"], ["bytes", "x"], ["formData", "a=1", URL_ENCODED],
    ["formData", '--x\\r\\nContent-Disposition: form-data; name="a"\\r\\n\\r\\n1\\r\\n--x--\\r\\n', MULTIPART]];
const each = async (run) => (await Promise.all(BODIES.map(run))).join("; ");
"""

_EVERY_READ = ("text", "json", "arrayBuffer", "blob", "bytes", "formData", "formData")


def _each(result: str) -> str:
    """What a realm script gives whose every read ends in `result`, with the read's name in place of "READ"."""
    return "; ".join(f"{read} {result.replace('READ', read)}" for read in _EVERY_READ)


# The scripts, with what Chrome 157.0.8103.79 gave for each in a page of its own.
_REALM_SCRIPTS: dict[str, tuple[str, str]] = {
    "a frame's method on the window's body": (
        """
        const w = frame().contentWindow;
        return each(async ([read, body, init]) => {
            const p = w.Response.prototype[read].call(new Response(body, init));
            return read + " " + promiseOf(p, w) + " " + of(await p, w);
        });
        """,
        _each("true/false true/false").replace("text true/false true/false", "text true/false string"),
    ),
    "the window's method on a frame's body": (
        """
        const w = frame().contentWindow;
        return each(async ([read, body, init]) => {
            const p = Response.prototype[read].call(new w.Response(body, init));
            return read + " " + promiseOf(p, w) + " " + of(await p, w);
        });
        """,
        _each("false/true false/true").replace("text false/true false/true", "text false/true string"),
    ),
    "a frame's method on the window's stream": (
        """
        const w = frame().contentWindow;
        return each(async ([read, body, init]) => {
            let controller;
            const stream = new w.ReadableStream({ start(c) { controller = c; } });
            const p = w.Response.prototype[read].call(new Response(stream, init));
            await new Promise((resolve) => setTimeout(resolve, 10));
            controller.enqueue(new TextEncoder().encode(body));
            controller.close();
            return read + " " + promiseOf(p, w) + " " + of(await p, w);
        });
        """,
        _each("true/false true/false").replace("text true/false true/false", "text true/false string"),
    ),
    "a frame's method on the window's stream that fails": (
        """
        const w = frame().contentWindow;
        const run = async (read, body, init) => {
            let controller;
            const stream = new w.ReadableStream({ start(c) { controller = c; } });
            const p = w.Response.prototype[read].call(new Response(stream, init));
            await new Promise((resolve) => setTimeout(resolve, 10));
            controller.enqueue(body === null ? "not bytes" : new TextEncoder().encode(body));
            if (body !== null) controller.close();
            return read + " " + promiseOf(p, w) + " " + await p.catch((e) => "rejected " + of(e, w));
        };
        return [await run("json", "{"), await run("formData", "x", MULTIPART), await run("formData", "x"),
                await run("text", null), await run("formData", null, URL_ENCODED)].join("; ");
        """,
        "json true/false rejected true/false; formData true/false rejected true/false; "
        "formData true/false rejected true/false; text true/false rejected true/false; "
        "formData true/false rejected true/false",
    ),
    "a frame's method on the window's null body": (
        """
        const w = frame().contentWindow;
        return each(async ([read, body, init]) => {
            const p = w.Response.prototype[read].call(new Response(null, init));
            return read + " " + promiseOf(p, w) + " " + await p.then((v) => of(v, w), (e) => "rejected " + of(e, w));
        });
        """,
        "text true/false string; json true/false rejected false/true; arrayBuffer true/false true/false; "
        "blob true/false true/false; bytes true/false true/false; formData true/false true/false; "
        "formData true/false rejected false/true",
    ),
    "a frame's method on the window's body that fails": (
        """
        const w = frame().contentWindow;
        const run = async ([read, body, init]) => {
            const p = w.Response.prototype[read].call(new Response(body, init));
            return read + " " + promiseOf(p, w) + " " + await p.catch((e) => "rejected " + of(e, w));
        };
        const used = new Response("x"); await used.text();
        const q = w.Response.prototype.text.call(used);
        return [await run(["json", "{"]), await run(["formData", "x", MULTIPART]), await run(["formData", "x"]),
                "used " + promiseOf(q, w) + " " + await q.catch((e) => "rejected " + of(e, w))].join("; ");
        """,
        "json true/false rejected true/false; formData true/false rejected true/false; "
        "formData true/false rejected true/false; used false/true rejected false/true",
    ),
    "the body of a frame that has gone": (
        """
        return each(async ([read, body, init]) => {
            const f = frame(); const r = new f.contentWindow.Response(body, init);
            f.remove();
            const p = Response.prototype[read].call(r);
            return read + " " + (p instanceof Promise) + " " + await outcome(p);
        });
        """,
        _each(
            "true rejected InvalidStateError: Failed to execute 'READ' on 'Response': "
            "Cannot load body from a frame or worker than has been detached"
        ),
    ),
    "the null body of a frame that has gone": (
        """
        return each(async ([read, body, init]) => {
            const f = frame(); const r = new f.contentWindow.Response(null, init);
            f.remove();
            const p = Response.prototype[read].call(r);
            return read + " " + (p instanceof Promise) + " " + await outcome(p);
        });
        """,
        _each("false pending"),
    ),
    "the method of a frame that has gone": (
        """
        return each(async ([read, body, init]) => {
            const f = frame(); const method = f.contentWindow.Response.prototype[read];
            f.remove();
            return read + " " + await outcome(method.call(new Response(body, init)));
        });
        """,
        "text ok x; json ok [object Object]; arrayBuffer ok [object ArrayBuffer]; blob ok [object Blob]; "
        "bytes ok 120; formData ok 1; formData ok 1",
    ),
    "the body of a frame that goes while it is read": (
        """
        return each(async ([read, body, init]) => {
            const f = frame(); const w = f.contentWindow; let controller;
            const stream = new w.ReadableStream({ start(c) { controller = c; } });
            const p = Response.prototype[read].call(new w.Response(stream, init));
            await new Promise((resolve) => setTimeout(resolve, 30));
            f.remove();
            try { controller.enqueue(new TextEncoder().encode(body)); controller.close(); } catch {}
            return read + " " + await outcome(p);
        });
        """,
        _each("pending"),
    ),
    "the method of a frame that goes while it reads": (
        """
        return each(async ([read, body, init]) => {
            const f = frame(); const w = f.contentWindow; let controller;
            const stream = new ReadableStream({ start(c) { controller = c; } });
            const p = w.Response.prototype[read].call(new Response(stream, init));
            await new Promise((resolve) => setTimeout(resolve, 30));
            f.remove();
            controller.enqueue(new TextEncoder().encode(body));
            controller.close();
            return read + " " + await outcome(p);
        });
        """,
        "text ok x; json ok [object Object]; arrayBuffer ok [object ArrayBuffer]; blob ok [object Blob]; "
        "bytes ok 120; formData ok 1; formData ok 1",
    ),
}


@pytest.mark.parametrize("name", _REALM_SCRIPTS)
def test_body_reads_are_of_the_realm_chrome_has_them_of(page_url: str, name: str) -> None:
    script, expected = _REALM_SCRIPTS[name]
    with (
        mitto.Session() as session,
        session._open_page(page_url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        wrapped = f"(async () => {{ {_REALM_PRELUDE} {script} }})()"
        assert page.evaluate(wrapped, timeout=_TIMEOUT_SECONDS) == expected
