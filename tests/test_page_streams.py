"""Tests of the Streams Standard in the Page program: docs/runtime.md, "Streams".

ReadableStream, WritableStream and TransformStream have Blink's behaviour, ported in native/dom from Blink's core/streams:
their controllers and readers, byte streams and BYOB readers, queuing strategies, tee(), pipeTo(), pipeThrough() and async
iteration, and the streams of encodings and compression. Each expected value is what Chrome 157.0.8103.79 gave for the
same script, in a page of its own. These run where MITTO_PAGE_PROGRAM names the Page program, as
tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 120

# Helpers that the scripts share: a log of what happened in order, and a read of a whole stream.
_PRELUDE = """
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
"""

# Each script and what Chrome 157.0.8103.79 gave for it, as _evaluate() writes it.
STREAMS = {
    # The interfaces and their shape.
    "constructors": (
        "return [ReadableStream, WritableStream, TransformStream, ReadableStreamDefaultReader,"
        " ReadableStreamBYOBReader, ByteLengthQueuingStrategy, CountQueuingStrategy].map((c) => c.name + c.length)"
        ".join()",
        "value:"
        " ReadableStream0,WritableStream0,TransformStream0,ReadableStreamDefaultReader1,ReadableStreamBYOBReader1,ByteLengthQueuingStrategy1,CountQueuingStrategy1",
    ),
    "controllers cannot be constructed": (
        "const out = []; for (const c of [ReadableStreamDefaultController, ReadableByteStreamController,"
        " ReadableStreamBYOBRequest, WritableStreamDefaultController, TransformStreamDefaultController]) {"
        " try { new c(); out.push('made'); } catch (e) { out.push(e.message); } } return out.join(' | ')",
        "value: Failed to construct 'ReadableStreamDefaultController': Illegal constructor | Failed to construct"
        " 'ReadableByteStreamController': Illegal constructor | Failed to construct 'ReadableStreamBYOBRequest':"
        " Illegal constructor | Failed to construct 'WritableStreamDefaultController': Illegal constructor | Failed"
        " to construct 'TransformStreamDefaultController': Illegal constructor",
    ),
    "async iterator": (
        "const s = new ReadableStream(); const it = s.values(); const proto = Object.getPrototypeOf(it);"
        " return [ReadableStream.prototype[Symbol.asyncIterator] === ReadableStream.prototype.values,"
        " Object.prototype.toString.call(it), Object.getOwnPropertyNames(proto).join(' '),"
        " Object.getPrototypeOf(proto) === Object.getPrototypeOf(Object.getPrototypeOf((async function* () {})"
        ".prototype)), s.locked].join()",
        "value: true,[object ReadableStream AsyncIterator],next return,true,true",
    ),
    "size functions": (
        "const a = new CountQueuingStrategy({ highWaterMark: 3 }); const b = new ByteLengthQueuingStrategy("
        "{ highWaterMark: 5 }); return [a.highWaterMark, a.size.name, a.size.length, a.size(), a.size ==="
        " new CountQueuingStrategy({ highWaterMark: 1 }).size, b.highWaterMark, b.size.name, b.size.length,"
        " b.size({ byteLength: 7 }), b.size === new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size].join()",
        "value: 3,size,0,1,true,5,size,1,7,true",
    ),
    "strategy without highWaterMark": (
        "try { new CountQueuingStrategy({}); } catch (e) { return e.constructor.name + ': ' + e.message; }",
        "value: TypeError: Failed to construct 'CountQueuingStrategy': Failed to read the 'highWaterMark' property"
        " from 'QueuingStrategyInit': Required member is undefined.",
    ),
    # Errors of the constructors, with Chrome's messages.
    "invalid type": (
        "new ReadableStream({ type: 'nope' })",
        "threw: RangeError: Failed to construct 'ReadableStream': Invalid type is specified",
    ),
    "pull that is not a function": (
        "new ReadableStream({ pull: 1 })",
        "threw: TypeError: Failed to construct 'ReadableStream': underlyingSource.pull must be a function or undefined",
    ),
    "byte source with a pull that is not a function": (
        "new ReadableStream({ type: 'bytes', pull: 1 })",
        "threw: TypeError: Failed to construct 'ReadableStream': Failed to read the 'pull' property from"
        " 'UnderlyingSource': The given value is not a function.",
    ),
    "byte source with size": (
        "new ReadableStream({ type: 'bytes' }, { size() { return 1; } })",
        "threw: RangeError: Failed to construct 'ReadableStream': Cannot create byte stream with size() defined on"
        " the strategy",
    ),
    "autoAllocateChunkSize 0": (
        "new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 0 })",
        "threw: TypeError: Failed to construct 'ReadableStream': autoAllocateChunkSize cannot be 0",
    ),
    "negative highWaterMark": (
        "new ReadableStream({}, { highWaterMark: -1 })",
        "threw: RangeError: Failed to construct 'ReadableStream': A queuing strategy's highWaterMark property must"
        " be a nonnegative, non-NaN number",
    ),
    "start that throws": (
        "new ReadableStream({ start() { throw new TypeError('mine'); } })",
        "threw: TypeError: mine",
    ),
    "writable sink write that is not a function": (
        "new WritableStream({ write: 'x' })",
        "threw: TypeError: Failed to construct 'WritableStream': underlyingSink.write must be a function or undefined",
    ),
    "transformer with readableType": (
        "new TransformStream({ readableType: 'bytes' })",
        "threw: RangeError: Failed to construct 'TransformStream': Invalid readableType was specified",
    ),
    # Reading.
    "default reader": (
        "const s = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });"
        " const r = s.getReader(); const first = await r.read(); const all = [first.value];"
        " for (;;) { const { value, done } = await r.read(); if (done) break; all.push(value); }"
        " return [Object.keys(first).join('/'), all.join(''), s.locked, await r.closed].join()",
        "value: done/value,ab,true,",
    ),
    "desiredSize and pull": (
        "let pulls = 0; let controller; const s = new ReadableStream({ start(c) { controller = c; },"
        " pull() { pulls++; } }, { highWaterMark: 2 }); await tick(); const before = [controller.desiredSize, pulls];"
        " controller.enqueue(1); controller.enqueue(2); await tick();"
        " return [before.join('/'), controller.desiredSize, pulls].join()",
        "value: 2/1,0,2",
    ),
    "enqueue after close": (
        "let c; new ReadableStream({ start(ctrl) { c = ctrl; } }); c.close();"
        " try { c.enqueue(1); } catch (e) { return e.constructor.name + ': ' + e.message; }",
        "value: TypeError: Failed to execute 'enqueue' on 'ReadableStreamDefaultController': Cannot enqueue a chunk"
        " into a readable stream that is closed or has been requested to be closed",
    ),
    "error rejects reads": (
        "const s = new ReadableStream({ start(c) { c.error(new Error('boom')); } }); const r = s.getReader();"
        " return [await settle(r.read()), await settle(r.closed)].join(' / ')",
        "value: rejected Error: boom / rejected Error: boom",
    ),
    "releaseLock rejects pending reads": (
        "const r = new ReadableStream().getReader(); const p = settle(r.read()); r.releaseLock();"
        " return [await p, await settle(r.closed)].join(' / ')",
        "value: rejected TypeError: Releasing Default reader / rejected TypeError: This readable stream reader has"
        " been released and cannot be used to monitor the stream's state",
    ),
    "locked stream": (
        "const s = new ReadableStream(); s.getReader();"
        " try { s.getReader(); } catch (e) { return [e.message, await settle(s.cancel())].join(' / '); }",
        "value: Failed to execute 'getReader' on 'ReadableStream': ReadableStreamDefaultReader constructor can only"
        " accept readable streams that are not yet locked to a reader / rejected TypeError: Failed to execute"
        " 'cancel' on 'ReadableStream': Cannot cancel a locked stream",
    ),
    "cancel": (
        "const s = new ReadableStream({ cancel(reason) { log.push('cancel ' + reason); return 'ignored'; } });"
        " const result = await s.cancel('why'); return [log.join(), result].join(' / ')",
        "value: cancel why / ",
    ),
    # As in Blink, a source's cancel() gets the controller after the reason, in a default and in a byte stream.
    "cancel gets the controller": (
        "const args = []; for (const type of [undefined, 'bytes']) { const s = new ReadableStream({ type,"
        " cancel(...a) { args.push([a.length, a[0], Object.prototype.toString.call(a[1])].join()); } });"
        " await s.cancel('why'); } return args.join(' / ')",
        "value: 2,why,[object ReadableStreamDefaultController] / 2,why,[object ReadableByteStreamController]",
    ),
    "custom size": (
        "let c; new ReadableStream({ start(ctrl) { c = ctrl; } }, { highWaterMark: 10, size(chunk) { return"
        " chunk.length; } }); c.enqueue('abcd'); const after = c.desiredSize; try { c.enqueue({ length: -1 }); }"
        " catch (e) { return [after, e.constructor.name, e.message].join(' / '); }",
        "value: 6 / RangeError / Failed to execute 'enqueue' on 'ReadableStreamDefaultController': The return value"
        " of a queuing strategy's size function must be a finite, non-NaN, non-negative number",
    ),
    # Byte streams.
    "byte stream with a default reader": (
        "const s = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(new Uint8Array([1, 2, 3]));"
        " c.close(); } }); const chunks = await readAll(s);"
        " return chunks.map((c) => Object.prototype.toString.call(c) + Array.from(c)).join()",
        "value: [object Uint8Array]1,2,3",
    ),
    "BYOB reader with min": (
        "const s = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(new Uint8Array([1, 2, 3]));"
        " c.enqueue(new Uint8Array([4, 5])); c.close(); } }); const r = s.getReader({ mode: 'byob' });"
        " const a = await r.read(new Uint16Array(2), { min: 2 }); const b = await r.read(new Uint8Array(4));"
        " return [a.value.constructor.name, a.value.length, Array.from(new Uint8Array(a.value.buffer)),"
        " b.done, b.value.length].join(' ')",
        "value: Uint16Array 2 1,2,3,4 false 1",
    ),
    "byobRequest": (
        "let request; const s = new ReadableStream({ type: 'bytes', pull(c) { request = c.byobRequest;"
        " request.view[0] = 9; request.respond(1); } }); const r = s.getReader({ mode: 'byob' });"
        " const { value } = await r.read(new Uint8Array(3)); return [value.length, value[0], request.view,"
        " Object.prototype.toString.call(request)].join(' ')",
        "value: 1 9  [object ReadableStreamBYOBRequest]",
    ),
    "autoAllocateChunkSize": (
        "const s = new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 4, pull(c) {"
        " const v = c.byobRequest.view; v[0] = 7; c.byobRequest.respond(1); c.close(); } });"
        " const r = s.getReader(); const { value } = await r.read();"
        " return [value.length, value[0], value.buffer.byteLength].join()",
        "value: 1,7,4",
    ),
    "enqueue an empty chunk": (
        "let c; new ReadableStream({ type: 'bytes', start(ctrl) { c = ctrl; } });"
        " try { c.enqueue(new Uint8Array(0)); } catch (e) { return e.message; }",
        "value: Failed to execute 'enqueue' on 'ReadableByteStreamController': chunk is empty",
    ),
    "BYOB read of a detached view": (
        "const r = new ReadableStream({ type: 'bytes' }).getReader({ mode: 'byob' }); const v = new Uint8Array(1);"
        " structuredClone(v.buffer, { transfer: [v.buffer] }); return await settle(r.read(v))",
        "value: rejected TypeError: Failed to execute 'read' on 'ReadableStreamBYOBReader': This readable stream"
        " reader cannot be used to read as the view has byte length equal to 0",
    ),
    # tee().
    "tee": (
        "const s = new ReadableStream({ start(c) { c.enqueue('x'); c.close(); } }); const [a, b] = s.tee();"
        " return [(await readAll(a)).join(), (await readAll(b)).join(), s.locked].join(' ')",
        "value: x x true",
    ),
    "tee of a byte stream": (
        "const s = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(new Uint8Array([5])); c.close(); } });"
        " const [a, b] = s.tee(); const byob = b.getReader({ mode: 'byob' });"
        " const ca = await readAll(a); const cb = await byob.read(new Uint8Array(2));"
        " return [ca[0][0], cb.value[0], cb.value.length, byob.constructor.name].join()",
        "value: 5,5,1,ReadableStreamBYOBReader",
    ),
    "tee cancels the source once both branches cancel": (
        "const s = new ReadableStream({ cancel(r) { log.push(JSON.stringify(r)); } }); const [a, b] = s.tee();"
        " const pa = a.cancel(1); await tick(); const early = log.length; await b.cancel(2); await pa;"
        " return [early, log.join()].join(' ')",
        "value: 0 [1,2]",
    ),
    # Async iteration.
    "for await": (
        "const s = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } }); const out = [];"
        " for await (const v of s) out.push(v); return [out.join(), s.locked].join(' ')",
        "value: 1,2 false",
    ),
    "break cancels": (
        "const s = new ReadableStream({ start(c) { c.enqueue(1); }, cancel(r) { log.push('cancel ' + r); } });"
        " for await (const v of s) { break; } return [log.join(), s.locked].join(' / ')",
        "value: cancel undefined / false",
    ),
    "preventCancel": (
        "const s = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); }, cancel() { log.push('cancel'); }"
        " }); for await (const v of s.values({ preventCancel: true })) { break; } const r = s.getReader();"
        " return [log.length, (await r.read()).value].join()",
        "value: 0,2",
    ),
    "next after return": (
        "const it = new ReadableStream().values(); const r = await it.return('v'); const n = await it.next();"
        " return [JSON.stringify(r), JSON.stringify(n)].join(' ')",
        'value: {"done":true,"value":"v"} {"done":true}',
    ),
    "next on another object": (
        "const it = new ReadableStream().values(); return await settle(it.next.call({}))",
        "value: rejected TypeError: Failed to execute 'next' on 'AsyncIterator_ReadableStream': Illegal invocation",
    ),
    # Writable streams.
    "writer": (
        "const s = new WritableStream({ write(chunk) { log.push('write ' + chunk); }, close() { log.push('close'); }"
        " }, { highWaterMark: 2 }); const w = s.getWriter(); const before = w.desiredSize; await w.ready;"
        " w.write('a'); const during = w.desiredSize; await w.write('b'); await w.close();"
        " return [before, during, log.join(), s.locked, w.desiredSize].join(' / ')",
        "value: 2 / 1 / write a,write b,close / true / 0",
    ),
    "abort": (
        "let signal; const s = new WritableStream({ start(c) { signal = c.signal; }, abort(r) { log.push('abort ' +"
        " r); } }); const w = s.getWriter(); await w.abort('why');"
        " return [log.join(), signal.aborted, signal.reason, await settle(w.closed)].join(' / ')",
        "value: abort why / true / why / rejected why",
    ),
    "write after close": (
        "const w = new WritableStream().getWriter(); w.close(); return await settle(w.write(1))",
        "value: rejected TypeError: Cannot write to a closing writable stream",
    ),
    "sink write rejects": (
        "const w = new WritableStream({ write() { return Promise.reject(new Error('no')); } }).getWriter();"
        " return [await settle(w.write(1)), await settle(w.closed), w.desiredSize].join(' / ')",
        "value: rejected Error: no / rejected Error: no / ",
    ),
    "controller error": (
        "let c; const s = new WritableStream({ start(ctrl) { c = ctrl; } }); const w = s.getWriter();"
        " c.error(new Error('e')); return [await settle(w.ready), await settle(w.write(1))].join(' / ')",
        "value: rejected Error: e / rejected Error: e",
    ),
    "releaseLock of a writer": (
        "const w = new WritableStream().getWriter(); w.releaseLock(); return [await settle(w.closed),"
        " await settle(w.write(1))].join(' / ')",
        "value: rejected TypeError: This writable stream writer has been released and cannot be used to monitor the"
        " stream's state / rejected TypeError: Failed to execute 'write' on 'WritableStreamDefaultWriter': This"
        " writable stream writer has been released and cannot be written to",
    ),
    # Transform streams.
    "transform": (
        "const t = new TransformStream({ transform(chunk, c) { c.enqueue(chunk.toUpperCase()); },"
        " flush(c) { c.enqueue('!'); } }); const w = t.writable.getWriter(); w.write('a'); w.write('b'); w.close();"
        " return (await readAll(t.readable)).join('')",
        "value: AB!",
    ),
    "identity transform": (
        "const t = new TransformStream(); const w = t.writable.getWriter(); w.write(1); w.close();"
        " return (await readAll(t.readable)).join()",
        "value: 1",
    ),
    "terminate": (
        "const t = new TransformStream({ start(c) { c.terminate(); } }); const w = t.writable.getWriter();"
        " return [await settle(w.write(1)), (await t.readable.getReader().read()).done].join(' / ')",
        "value: rejected TypeError: The transform stream has been terminated / true",
    ),
    "transform throws": (
        "const t = new TransformStream({ transform() { throw new Error('bad'); } });"
        " const read = settle(t.readable.getReader().read()); const w = t.writable.getWriter();"
        " return [await settle(w.write(1)), await read].join(' / ')",
        "value: rejected Error: bad / rejected Error: bad",
    ),
    # Piping.
    "pipeTo": (
        "const s = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } });"
        " const out = []; await s.pipeTo(new WritableStream({ write(chunk) { out.push(chunk); },"
        " close() { out.push('closed'); } })); return out.join()",
        "value: 1,2,closed",
    ),
    "pipeThrough": (
        "const s = new ReadableStream({ start(c) { c.enqueue('a'); c.close(); } });"
        " const t = new TransformStream({ transform(chunk, c) { c.enqueue(chunk + chunk); } });"
        " return (await readAll(s.pipeThrough(t))).join()",
        "value: aa",
    ),
    "pipeTo with a signal": (
        "const controller = new AbortController(); const s = new ReadableStream({ cancel(r) {"
        " log.push('cancel ' + r.name); } }); const p = settle(s.pipeTo(new WritableStream({ abort(r) {"
        " log.push('abort ' + r.name); } }), { signal: controller.signal })); controller.abort();"
        " return [await p, log.join()].join(' / ')",
        "value: rejected AbortError: signal is aborted without reason / abort AbortError,cancel AbortError",
    ),
    "pipeTo a locked stream": (
        "const w = new WritableStream(); w.getWriter(); return await settle(new ReadableStream().pipeTo(w))",
        "value: rejected TypeError: Failed to execute 'pipeTo' on 'ReadableStream': Cannot pipe to a locked stream",
    ),
    "pipeThrough without a pair": (
        "new ReadableStream().pipeThrough({})",
        "threw: TypeError: Failed to execute 'pipeThrough' on 'ReadableStream': Failed to read the 'readable'"
        " property from 'ReadableWritablePair': Required member is undefined.",
    ),
    "preventClose": (
        "const s = new ReadableStream({ start(c) { c.close(); } }); const ws = new WritableStream({ close() {"
        " log.push('close'); } }); await s.pipeTo(ws, { preventClose: true }); return [log.length, ws.locked].join()",
        "value: 0,false",
    ),
    # Encoding and compression streams.
    "TextEncoderStream": (
        "const t = new TextEncoderStream(); const w = t.writable.getWriter(); w.write('a\\u00e9'); w.write('\\ud83d');"
        " w.write('\\ude00!'); w.write('\\ud800'); w.close(); const chunks = await readAll(t.readable);"
        " return [t.encoding, chunks.map((c) => c.constructor.name + ':' + Array.from(c).join(' ')).join(' | ')]"
        ".join(' / ')",
        "value: utf-8 / Uint8Array:97 195 169 | Uint8Array:240 159 152 128 33 | Uint8Array:239 191 189",
    ),
    "TextDecoderStream": (
        "const t = new TextDecoderStream(); const w = t.writable.getWriter(); w.write(new Uint8Array([0xef, 0xbb]));"
        " w.write(new Uint8Array([0xbf, 0x61, 0xe2, 0x82])); w.write(new Uint8Array([0xac]).buffer); w.close();"
        " const chunks = await readAll(t.readable); return [t.encoding, t.fatal, t.ignoreBOM, JSON.stringify(chunks)]"
        ".join(' / ')",
        'value: utf-8 / false / false / ["a","€"]',
    ),
    "TextDecoderStream labels": (
        "return ['latin1', ' UTF-16 ', 'ascii', 'utf8'].map((l) => new TextDecoderStream(l).encoding).join()",
        "value: windows-1252,utf-16le,windows-1252,utf-8",
    ),
    "TextDecoderStream invalid label": (
        "new TextDecoderStream('nope')",
        "threw: RangeError: Failed to construct 'TextDecoderStream': The encoding label provided ('nope') is invalid.",
    ),
    "TextDecoderStream fatal": (
        "const t = new TextDecoderStream('utf-8', { fatal: true }); const w = t.writable.getWriter();"
        " const read = settle(t.readable.getReader().read()); return [await settle(w.write(new Uint8Array([0xff]))),"
        " await read].join(' / ')",
        "value: rejected TypeError: The encoded data is not valid. / rejected TypeError: The encoded data is not"
        " valid.",
    ),
    "TextDecoderStream with a string chunk": (
        "const t = new TextDecoderStream(); const w = t.writable.getWriter();"
        " const read = settle(t.readable.getReader().read()); return [await settle(w.write('x')), await read]"
        ".join(' / ')",
        "value: rejected TypeError: The provided value is not of type '(ArrayBuffer or ArrayBufferView)'. / rejected"
        " TypeError: The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.",
    ),
    "compression round trip": (
        "const input = new TextEncoder().encode('hello hello hello hello'); const out = [];"
        " for (const format of ['gzip', 'deflate', 'deflate-raw']) { const c = new CompressionStream(format);"
        " const w = c.writable.getWriter(); w.write(input); w.close(); const parts = await readAll(c.readable);"
        " const d = new DecompressionStream(format); const dw = d.writable.getWriter(); for (const p of parts)"
        " dw.write(p); dw.close(); const back = await readAll(d.readable); out.push(format + ':' + parts.length + ':'"
        " + new TextDecoder().decode(back[0])); } return out.join(' ')",
        "value: gzip:2:hello hello hello hello deflate:2:hello hello hello hello deflate-raw:1:hello hello hello hello",
    ),
    # Mac Chrome's header ends in 19, the operating system that its zlib was built for, and Windows Chrome's in 10,
    # which a Page writes on every Host.
    "gzip bytes": (
        "const c = new CompressionStream('gzip'); const w = c.writable.getWriter(); w.write(new Uint8Array([1, 2, 3]));"
        " w.close(); const parts = await readAll(c.readable); return parts.map((p) => Array.from(p).join(',')).join(' | ')",
        "value: 31,139,8,0,0,0,0,0,0,10 | 99,100,98,6,0,29,128,188,85,3,0,0,0",
    ),
    "unsupported compression format": (
        "new CompressionStream('br')",
        "threw: TypeError: Failed to construct 'CompressionStream': Unsupported compression format: 'br'",
    ),
    "truncated compressed input": (
        "const d = new DecompressionStream('deflate'); const w = d.writable.getWriter();"
        " const read = settle(readAll(d.readable)); w.write(new Uint8Array([0x78, 0x9c])); return [await settle(w.close()),"
        " await read].join(' / ')",
        "value: rejected TypeError: Compressed input was truncated. / rejected TypeError: Compressed input was"
        " truncated.",
    ),
    "invalid compressed input": (
        "const d = new DecompressionStream('gzip'); const w = d.writable.getWriter();"
        " const read = settle(readAll(d.readable)); return [await settle(w.write(new Uint8Array([1, 2, 3, 4]))),"
        " await read].join(' / ')",
        "value: rejected TypeError: The compressed data was not valid: incorrect header check. / rejected TypeError:"
        " The compressed data was not valid: incorrect header check.",
    ),
    # The order of reactions, which promises drive.
    "order of reads and enqueues": (
        "let c; const s = new ReadableStream({ start(ctrl) { c = ctrl; } }); const r = s.getReader();"
        " r.read().then(({ value }) => log.push('read ' + value)); Promise.resolve().then(() => log.push('micro'));"
        " c.enqueue(1); log.push('enqueued'); await tick(); return log.join()",
        "value: enqueued,micro,read 1",
    ),
}


@pytest.fixture(scope="module")
def program() -> Path:
    value = os.environ.get(_PROGRAM_ENV_VAR)
    if not value:
        pytest.skip(f"{_PROGRAM_ENV_VAR} is not set")
    return Path(value)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_text_encoding.py writes it.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {_PRELUDE} {script} }})();
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


@pytest.mark.parametrize(("script", "chrome"), STREAMS.values(), ids=STREAMS.keys())
def test_streams_do_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome
