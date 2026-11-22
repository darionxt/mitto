"""Tests of the File API in the Page program: docs/runtime.md, "The File API".

Blob, File, FileReader and ProgressEvent have Blink's behaviour, ported in native/dom from Blink's core/fileapi: a
blob's bytes made from strings, buffers and other blobs, slices, reads as text, an ArrayBuffer, bytes or a stream,
FileReader's reads and events, blobs in structured clones, and blobs as the bodies of Response and Request. Each
expected value is what Chrome 157.0.8103.79 gave for the same script, in a page of its own, but where a comment says
that Windows Chrome differs from the Mac's, since every Persona is of Windows. These run where MITTO_PAGE_PROGRAM names
the Page program, as tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 120

# Helpers that the scripts share: a log of what happened in order, a read of a whole stream, and a FileReader's read
# with each event it fires.
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
const read = (method, blob, ...args) => new Promise((resolve) => {
    const reader = new FileReader();
    const events = [];
    for (const type of ["loadstart", "progress", "load", "abort", "error", "loadend"]) {
        reader.addEventListener(type, (e) => events.push(
            type + " " + e.lengthComputable + " " + e.loaded + "/" + e.total + " " + reader.readyState));
    }
    reader.onloadend = () => resolve({ reader, events });
    reader[method](blob, ...args);
});
"""

# Each script and what Chrome 157.0.8103.79 gave for it, as _evaluate() writes it.
FILE_API = {
    "constructors": (
        "return [Blob, File, FileReader, ProgressEvent].map((c) => c.name + c.length).join()",
        "value: Blob0,File2,FileReader0,ProgressEvent1",
    ),
    "an empty blob": (
        "const b = new Blob(); return [b.size, JSON.stringify(b.type), await b.text() === '', (await"
        " b.arrayBuffer()).byteLength].join()",
        'value: 0,"",true,0',
    ),
    "the parts of a blob": (
        "const inner = new Blob(['in']); const view = new Uint16Array([0x4241]);"
        " const b = new Blob(['a\\ud800b', new Uint8Array([0x43]).buffer, view, new DataView(new Uint8Array([0x44,"
        " 0x45]).buffer, 1), inner, 7]);"
        " return [b.size, JSON.stringify(await b.text())].join(' / ')",
        'value: 12 / "a�bCABEin7"',
    ),
    "a blob's type": (
        "return [new Blob([], { type: 'Text/HTML;Charset=UTF-8' }).type, JSON.stringify(new Blob([], { type:"
        " 'a\\u00e9' }).type),"
        " JSON.stringify(new Blob([], { type: 'a\\u0019b' }).type), new Blob([], { type: ' x ' }).type.length].join('"
        " / ')",
        'value: text/html;charset=utf-8 / "" / "" / 3',
    ),
    "native line endings": (
        # Windows Chrome writes CRLF, as Blink's NormalizeLineEndingsToNative() does on Windows; Mac Chrome gave LF.
        "const b = new Blob(['a\\nb\\rc\\r\\nd\\n\\r'], { endings: 'native' }); const t = new Blob(['a\\nb'], {"
        " endings: 'transparent' });"
        " return [JSON.stringify(await b.text()), JSON.stringify(await t.text())].join(' / ')",
        'value: "a\\r\\nb\\r\\nc\\r\\nd\\r\\n\\r\\n" / "a\\nb"',
    ),
    "an invalid endings": (
        "new Blob([], { endings: 'crlf' })",
        "threw: TypeError: Failed to construct 'Blob': Failed to read the 'endings' property from 'BlobPropertyBag':"
        " The provided value 'crlf' is not a valid enum value of type EndingType.",
    ),
    "a part that is not a sequence": (
        "new Blob('abc')",
        "threw: TypeError: Failed to construct 'Blob': The provided value cannot be converted to a sequence.",
    ),
    "slices": (
        "const b = new Blob(['0123456789'], { type: 'a/b' });"
        " const s = [b.slice(), b.slice(2), b.slice(-3), b.slice(2, -2), b.slice(8, 2), b.slice(20), b.slice(-20, 3),"
        " b.slice(1.5, 3.5), b.slice(NaN, '4')];"
        " const texts = await Promise.all(s.map((x) => x.text())); return texts.join(',') + ' / ' + s.map((x) =>"
        " JSON.stringify(x.type)).join(',') + ' / '"
        " + b.slice(0, 1, 'X/Y').type + ' ' + (await b.slice(2, 8).slice(1, 3).text())",
        'value: 0123456789,23456789,789,234567,,,012,23,0123 / "","","","","","","","","" / x/y 34',
    ),
    "slice's arguments": (
        "const b = new Blob(['0123456789']); return (await Promise.all([b.slice(-Infinity, Infinity), b.slice(2.5,"
        " 4.5), b.slice(-1.5),"
        " b.slice(2 ** 60, 2 ** 61)].map((x) => x.text()))).join()",
        "value: 0123456789,23,89,",
    ),
    "text() decodes UTF-8": (
        "const b = new Blob([new Uint8Array([0xEF, 0xBB, 0xBF, 0x68, 0xC3, 0xA9, 0xFF, 0x21])]); return"
        " JSON.stringify(await b.text())",
        'value: "hé�!"',
    ),
    "text() and a UTF-16 byte order mark": (
        "const b = new Blob([new Uint8Array([0xFF, 0xFE, 0x68, 0x00, 0x69, 0x00])]); return JSON.stringify(await"
        " b.text())",
        'value: "hi"',
    ),
    "arrayBuffer() and bytes()": (
        "const b = new Blob(['ab', new Uint8Array([1, 2])]); const ab = await b.arrayBuffer(); const u = await"
        " b.bytes();"
        " return [Object.prototype.toString.call(ab), Array.from(new Uint8Array(ab)),"
        " Object.prototype.toString.call(u), Array.from(u),"
        " u.byteOffset, u.buffer.byteLength].join(' / ')",
        "value: [object ArrayBuffer] / 97,98,1,2 / [object Uint8Array] / 97,98,1,2 / 0 / 4",
    ),
    "reads settle from later tasks": (
        "const b = new Blob(['x']); b.text().then(() => log.push('text')); b.arrayBuffer().then(() =>"
        " log.push('arrayBuffer'));"
        " Promise.resolve().then(() => log.push('micro')); setTimeout(() => log.push('timeout'), 0);"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return log.join()",
        "value: micro,timeout,text,arrayBuffer",
    ),
    "stream()": (
        "const b = new Blob(['hello', ' ', 'world']); const s = b.stream(); const chunks = await readAll(s);"
        " return [Object.prototype.toString.call(s), s === b.stream(), chunks.map((c) =>"
        " Object.prototype.toString.call(c)).join(),"
        " new TextDecoder().decode(new Uint8Array(chunks.flatMap((c) => Array.from(c))))].join(' / ')",
        "value: [object ReadableStream] / false / [object Uint8Array] / hello world",
    ),
    "stream() of an empty blob": (
        "const reader = new Blob().stream().getReader(); const first = await reader.read(); return [first.done,"
        " String(first.value)].join()",
        "value: true,undefined",
    ),
    "stream() with a BYOB reader": (
        "const reader = new Blob(['abcdef']).stream().getReader({ mode: 'byob' }); const a = await reader.read(new"
        " Uint8Array(4));"
        " const b = await reader.read(new Uint8Array(4)); const c = await reader.read(new Uint8Array(4));"
        " return [Array.from(a.value), Array.from(b.value), c.done].join(' / ')",
        "value: 97,98,99,100 / 101,102 / true",
    ),
    "stream() of a large blob": (
        "const b = new Blob([new Uint8Array(5 * 1024 * 1024)]); const chunks = await readAll(b.stream());"
        " return chunks.reduce((n, c) => n + c.byteLength, 0)",
        "value: 5242880",
    ),
    "textStream()": (
        "const s = new Blob([new Uint8Array([0xEF, 0xBB, 0xBF, 0x68, 0xC3, 0xA9])]).textStream(); return"
        " JSON.stringify(await readAll(s))",
        'value: ["hé"]',
    ),
    "a file": (
        "const before = Date.now(); const f = new File(['ab', new Blob(['c'])], 'n.txt', { type: 'A/B' }); const after"
        " = Date.now();"
        " return [f.name, f.size, f.type, f.lastModified >= before && f.lastModified <= after, f.webkitRelativePath"
        " === '',"
        " Object.prototype.toString.call(f.lastModifiedDate), f.lastModifiedDate.getTime() === f.lastModified, f"
        " instanceof Blob,"
        " await f.text()].join(' / ')",
        "value: n.txt / 3 / a/b / true / true / [object Date] / true / true / abc",
    ),
    "a file's lastModified": (
        "return [new File([], 'a', { lastModified: 42 }).lastModified, new File([], 'a', { lastModified: -1.5"
        " }).lastModified,"
        " new File([], 'a', { lastModified: 2 ** 53 }).lastModified, new File([], 'a', { lastModified: 'x'"
        " }).lastModified].join()",
        "value: 42,-1,9007199254740992,0",
    ),
    "a file needs a name": (
        "new File([])",
        "threw: TypeError: Failed to construct 'File': 2 arguments required, but only 1 present.",
    ),
    "a file's name": (
        "return [new File([], 'a/b\\\\c').name, JSON.stringify(new File([], '\\ud800').name), new File([],"
        " 1).name].join(' / ')",
        'value: a/b\\c / "�" / 1',
    ),
    "a slice of a file is a blob": (
        "const s = new File(['abc'], 'n').slice(1); return [Object.prototype.toString.call(s), s instanceof File,"
        " await s.text()].join()",
        "value: [object Blob],false,bc",
    ),
    "readAsText()": (
        "const { reader, events } = await read('readAsText', new Blob(['h\\u00e9'])); return [events.join(', '),"
        " reader.result, reader.error].join(' / ')",
        "value: loadstart true 0/3 1, progress true 3/3 1, load true 3/3 2, loadend true 3/3 2 / hé / ",
    ),
    "readAsText() with an encoding": (
        "const a = await read('readAsText', new Blob([new Uint8Array([0x68, 0x00, 0xE9, 0x00])]), 'utf-16le');"
        " const b = await read('readAsText', new Blob([new Uint8Array([0xEF, 0xBB, 0xBF, 0x68])]), 'utf-16le');"
        " const c = await read('readAsText', new Blob([new Uint8Array([0xE9])]), 'nope');"
        " const d = await read('readAsText', new Blob([new Uint8Array([0xE9])], { type:"
        " 'text/plain;charset=windows-1252' }));"
        " return [a, b, c, d].map((x) => JSON.stringify(x.reader.result)).join(' / ')",
        'value: "hé" / "h" / "�" / "�"',
    ),
    "readAsBinaryString()": (
        "const { reader } = await read('readAsBinaryString', new Blob([new Uint8Array([0x61, 0xE9, 0xFF])])); return"
        " JSON.stringify(reader.result)",
        'value: "aéÿ"',
    ),
    "readAsDataURL()": (
        "const a = await read('readAsDataURL', new Blob(['hi'], { type: 'text/plain;charset=utf-8' }));"
        " const b = await read('readAsDataURL', new Blob(['hi'])); const c = await read('readAsDataURL', new Blob());"
        " return [a, b, c].map((x) => x.reader.result).join(' / ')",
        "value: data:text/plain;charset=utf-8;base64,aGk= / data:application/octet-stream;base64,aGk= /"
        " data:application/octet-stream;base64,",
    ),
    "readAsArrayBuffer()": (
        "const { reader, events } = await read('readAsArrayBuffer', new Blob(['abc']));"
        " return [events.join(', '), Object.prototype.toString.call(reader.result), reader.result === reader.result,"
        " Array.from(new Uint8Array(reader.result))].join(' / ')",
        "value: loadstart true 0/3 1, progress true 3/3 1, load true 3/3 2, loadend true 3/3 2 / [object ArrayBuffer]"
        " / true / 97,98,99",
    ),
    "reading an empty blob": (
        "const { events, reader } = await read('readAsText', new Blob()); return [events.join(', '),"
        " JSON.stringify(reader.result)].join(' / ')",
        'value: loadstart true 0/0 1, load true 0/0 2, loadend true 0/0 2 / ""',
    ),
    "a reader's states": (
        "const reader = new FileReader(); log.push(reader.readyState, reader.result, reader.error);"
        " reader.readAsText(new Blob(['x'])); log.push(reader.readyState, reader.result);"
        " reader.onload = () => log.push('load', reader.readyState, reader.result);"
        " await new Promise((resolve) => { reader.onloadend = resolve; }); log.push(reader.readyState,"
        " FileReader.EMPTY, FileReader.LOADING, FileReader.DONE);"
        " return log.join()",
        "value: 0,,,1,,load,2,x,2,0,1,2",
    ),
    "a second read while one runs": (
        "const reader = new FileReader(); reader.readAsText(new Blob(['x'])); try { reader.readAsText(new"
        " Blob(['y'])); }"
        " catch (e) { return [e.name, e.message, reader.readyState].join(' / '); }",
        "value: InvalidStateError / Failed to execute 'readAsText' on 'FileReader': The object is already busy reading"
        " Blobs. / 1",
    ),
    "a second readAsText() that throws still sets the encoding": (
        # As Blink's readAsText() keeps the encoding before ReadInternal() checks the state, which Chrome 157 shows:
        # the read under way decodes with the encoding of the call that threw.
        "const r = new FileReader(); r.readAsText(new Blob([new Uint8Array([0xE9])]), 'windows-1252'); let thrown = '';"
        " try { r.readAsText(new Blob(['x'])); } catch (e) { thrown = e.name; }"
        " await new Promise((res) => { r.onloadend = res; }); return [thrown, JSON.stringify(r.result)].join(' / ')",
        'value: InvalidStateError / "\ufffd"',
    ),
    "abort()": (
        "const reader = new FileReader(); for (const type of ['loadstart', 'load', 'abort', 'error', 'loadend'])"
        " reader.addEventListener(type, (e) => log.push(type + ' ' + e.loaded + '/' + e.total));"
        " reader.readAsText(new Blob(['abc'])); reader.abort(); log.push(reader.readyState, reader.result,"
        " reader.error.name, reader.error.message);"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return log.join(' | ')",
        "value: abort 0/0 | loadend 0/0 | 2 |  | AbortError | An ongoing operation was aborted, typically with a call"
        " to abort().",
    ),
    "abort() in loadstart": (
        "const reader = new FileReader(); reader.onloadstart = () => reader.abort(); for (const type of ['load',"
        " 'abort', 'error', 'loadend'])"
        " reader.addEventListener(type, (e) => log.push(type + ' ' + e.loaded + '/' + e.total)); reader.readAsText(new"
        " Blob(['abc']));"
        " await new Promise((resolve) => setTimeout(resolve, 50)); return log.join(' | ')",
        "value: abort 0/0 | loadend 0/0",
    ),
    "abort() before a read": (
        "const reader = new FileReader(); let fired = false; reader.onabort = () => { fired = true; }; reader.abort();"
        " return [reader.readyState, fired, reader.error].join()",
        "value: 0,false,",
    ),
    "a read from loadend": (
        "const reader = new FileReader(); let reads = 0; reader.onloadend = () => { log.push(reader.result); if"
        " (++reads === 1) reader.readAsText(new Blob(['second'])); };"
        " reader.readAsText(new Blob(['first'])); await new Promise((resolve) => setTimeout(resolve, 50)); return"
        " log.join()",
        "value: first,second",
    ),
    "progress events": (
        "const e = new ProgressEvent('x'); const f = new ProgressEvent('y', { lengthComputable: true, loaded: 2.5,"
        " total: 7, bubbles: true });"
        " return [e.lengthComputable, e.loaded, e.total, e.bubbles, f.lengthComputable, f.loaded, f.total, f.bubbles,"
        " e instanceof Event].join()",
        "value: false,0,0,false,true,2.5,7,true,true",
    ),
    "a reader's events": (
        "const reader = new FileReader(); let event; reader.onload = (e) => { event = e; }; reader.readAsText(new"
        " Blob(['ab']));"
        " await new Promise((resolve) => { reader.onloadend = resolve; }); return"
        " [Object.prototype.toString.call(event), event.isTrusted,"
        " event.bubbles, event.cancelable, event.target === reader].join()",
        "value: [object ProgressEvent],true,false,false,true",
    ),
    "structured clones of blobs": (
        "const b = new Blob(['abc'], { type: 'x/y' }); const f = new File(['de'], 'f.txt', { type: 'a/b',"
        " lastModified: 1234 });"
        " const c = structuredClone({ b, f, s: b.slice(1) }); return [c.b === b, c.b.size, c.b.type, await c.b.text(),"
        " c.f instanceof File,"
        " c.f.name, c.f.type, c.f.lastModified, await c.f.text(), await c.s.text()].join(' / ')",
        "value: false / 3 / x/y / abc / true / f.txt / a/b / 1234 / de / bc",
    ),
    "a blob through a MessagePort": (
        "const { port1, port2 } = new MessageChannel(); const got = new Promise((resolve) => { port2.onmessage = (e)"
        " => resolve(e.data); });"
        " port1.postMessage(new File(['zz'], 'm', { lastModified: 5 })); const f = await got; return [f.name,"
        " f.lastModified, await f.text()].join()",
        "value: m,5,zz",
    ),
    "createObjectURL()": (
        "const u = URL.createObjectURL(new Blob(['x'])); const rest = u.slice(('blob:' + self.origin + '/').length);"
        " return [u.startsWith('blob:' + self.origin + '/'),"
        " /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(rest),"
        " u === URL.createObjectURL(new Blob(['x'])), URL.revokeObjectURL(u), URL.revokeObjectURL('nope'),"
        " URL.createObjectURL.length, URL.revokeObjectURL.length].join()",
        "value: true,true,false,,,1,1",
    ),
    "createObjectURL()'s argument": (
        "const out = []; for (const f of [() => URL.createObjectURL({}), () => URL.createObjectURL(null),"
        " () => URL.createObjectURL(), () => URL.revokeObjectURL()]) { try { f(); } catch (e) { out.push(e.message); }"
        " }"
        " return out.join(' | ')",
        "value: Failed to execute 'createObjectURL' on 'URL': Overload resolution failed. | Failed to execute"
        " 'createObjectURL' on 'URL': Overload resolution failed. | Failed to execute 'createObjectURL' on 'URL': 1"
        " argument required, but only 0 present. | Failed to execute 'revokeObjectURL' on 'URL': 1 argument required,"
        " but only 0 present.",
    ),
    "a blob as a response's body": (
        "const r = new Response(new Blob(['abc'], { type: 'Text/Plain' })); const e = new Response(new Blob(['x']));"
        " return [r.headers.get('content-type'), e.headers.get('content-type'), await r.text()].join(' / ')",
        "value: text/plain /  / abc",
    ),
    "blob() of a response": (
        "const r = new Response('abc', { headers: { 'Content-Type': 'Text/HTML; Charset=UTF-8' } }); const b = await"
        " r.blob();"
        " const n = await new Response(null).blob(); const u = await new Response('x', { headers: { 'content-type':"
        " 'a\\u0001' } }).blob();"
        " return [Object.prototype.toString.call(b), b.type, await b.text(), r.bodyUsed, JSON.stringify(n.type),"
        " n.size, JSON.stringify(u.type)].join(' / ')",
        'value: [object Blob] / text/html; charset=utf-8 / abc / true / "" / 0 / ""',
    ),
    "bytes() of a response": (
        "const r = new Response('ab'); const u = await r.bytes(); const n = await new Response(null).bytes();"
        " return [Object.prototype.toString.call(u), Array.from(u), r.bodyUsed, n.byteLength].join(' / ')",
        "value: [object Uint8Array] / 97,98 / true / 0",
    ),
    "a blob as a request's body": (
        "const q = new Request('https://example.com/', { method: 'POST', body: new Blob(['hi'], { type: 'a/b' }) });"
        " const b = await q.blob(); return [q.headers.get('content-type'), b.type, await b.text(), q.bodyUsed].join('"
        " / ')",
        "value: a/b / a/b / hi / true",
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


@pytest.mark.parametrize(("script", "chrome"), FILE_API.values(), ids=FILE_API.keys())
def test_the_file_api_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome
