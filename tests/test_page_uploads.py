"""Tests of a fetch() whose body is a stream, which a Page sends as its Session reads it: docs/runtime.md, "fetch()".

The capture server stands in for the Internet, over HTTP/1.1, HTTP/2 and HTTP/3, as it does for tests/test_streaming.py.
Each expected value is what Chrome 157.0.8103.79 gave for the same script, at a page of the same server, and the request
of the Recording Case's own stream is held to Chrome's Baseline of it. Each test goes through both APIs, and needs a native
library with the Page program beside it, as CI's native job installs it on every Host.
"""

import json
from collections.abc import Iterator
from typing import Any, cast

import pytest

import mitto
from mitto import _native
from mitto._persona import ENGINE_RELEASE_PERSONA, load_persona
from tests.page_server import FAVICON_PATH
from tools.capture import CASES, HTTP1, HTTP2, HTTP3, CaptureServer, normal_form
from tools.capture.cases import CROSS_SITE_HOST
from tools.capture.http1 import Http1Request
from tools.capture.http2 import Http2Request
from tools.capture.http3 import Http3Request
from tools.parity import BASELINES_DIRECTORY
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("both_apis")

# Longer than any local load takes, even on a busy runner.
_TIMEOUT_SECONDS = 30.0

# The Recording Case of a page's stream, and a page of the same origin to run the scripts at, whose answer is text.
_CASE = "fetch_post_stream"
_PAGE_PATH = "/case/fetch-get-same-site"

# Helpers that the scripts share: a body of a stream, which needs duplex, and a stream of chunks that closes.
_UPLOAD_PRELUDE = (
    "const enc = new TextEncoder();"
    " const settle = (promise) => promise.then((v) => 'fulfilled ' + String(v), (e) => 'rejected ' + String(e));"
    " const tick = () => new Promise((resolve) => setTimeout(resolve, 0));"
    " const post = (body, init = {}) => ({ method: 'POST', duplex: 'half', body, ...init });"
    " const stream = (...chunks) => new ReadableStream({ start(c) { for (const chunk of chunks) c.enqueue(chunk);"
    " c.close(); } });"
    f" const crossSite = 'https://{CROSS_SITE_HOST}:' + location.port;"
)

# Each script and what Chrome 157.0.8103.79 gave for it over HTTP/2 and over HTTP/3, as _run() writes it.
_UPLOADS = {
    "a stream body": (
        "let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; if (pulls <= 3) "
        "c.enqueue(enc.encode('part' + pulls)); else c.close(); } }); const r = await fetch('/case/fetch-post-stream', "
        "post(body)); return [r.status, JSON.stringify(await r.text()), pulls, body.locked].join(' / ')",
        'value: 200 / "answered\\n" / 4 / true',
    ),
    "an empty stream body": (
        "const body = stream(); const r = await fetch('/case/fetch-post-stream', post(body)); return [r.status, "
        "body.locked].join()",
        "value: 200,true",
    ),
    "an empty chunk": (
        "const r = await fetch('/case/fetch-post-stream', post(stream(new Uint8Array(0), enc.encode('x'), new "
        "Uint8Array(0)))); return r.status",
        "value: 200",
    ),
    "a chunk of text": (
        "return await settle(fetch('/case/fetch-post-stream', post(stream('text'))))",
        "value: rejected TypeError: Failed to fetch",
    ),
    "a chunk that is an ArrayBuffer": (
        "return await settle(fetch('/case/fetch-post-stream', post(stream(new ArrayBuffer(2)))))",
        "value: rejected TypeError: Failed to fetch",
    ),
    "a chunk that is a DataView": (
        "return await settle(fetch('/case/fetch-post-stream', post(stream(new DataView(new ArrayBuffer(2))))))",
        "value: rejected TypeError: Failed to fetch",
    ),
    "a chunk that is a Uint16Array": (
        "return await settle(fetch('/case/fetch-post-stream', post(stream(new Uint16Array(2)))))",
        "value: rejected TypeError: Failed to fetch",
    ),
    "a chunk of a view into a larger buffer": (
        "const view = new Uint8Array(new TextEncoder().encode('abcdef').buffer, 2, 3); const r = await "
        "fetch('/case/fetch-post-stream', post(stream(view))); return r.status",
        "value: 200",
    ),
    "a stream that errors": (
        "let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; if (pulls === 1) "
        "c.enqueue(enc.encode('x')); else c.error(new Error('bad')); } }); return [await "
        "settle(fetch('/case/fetch-post-stream', post(body))), pulls].join(' / ')",
        "value: rejected TypeError: Failed to fetch / 2",
    ),
    "a stream that has errored": (
        "const body = new ReadableStream({ start(c) { c.error(new Error('bad')); } }); return [await "
        "settle(fetch('/case/fetch-post-stream', post(body))), body.locked].join(' / ')",
        "value: rejected TypeError: Failed to fetch / true",
    ),
    "a stream that a script cancels": (
        "let pulls = 0; let cancelled = 'no'; const body = new ReadableStream({ pull(c) { pulls++; "
        "c.enqueue(enc.encode('x')); if (pulls === 2) c.close(); }, cancel(reason) { cancelled = String(reason); } }); "
        "const r = await fetch('/case/fetch-post-stream', post(body)); await tick(); return [r.status, pulls, "
        "cancelled].join(' / ')",
        "value: 200 / 2 / no",
    ),
    "a Request's stream body": (
        "const body = stream(enc.encode('x')); const q = new Request('/case/fetch-post-stream', post(body)); const r = "
        "await fetch(q); return [r.status, q.bodyUsed, body.locked].join()",
        "value: 200,true,true",
    ),
    "a Request from a Request with a stream body": (
        "const body = stream(enc.encode('x')); const a = new Request('/case/fetch-post-stream', post(body)); const b = "
        "new Request(a); const r = await fetch(b); return [r.status, a.bodyUsed, b.bodyUsed].join()",
        "value: 200,true,true",
    ),
    "a keepalive fetch of a Request with a stream body": (
        "let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; c.enqueue(enc.encode('x')); if (pulls "
        "=== 2) c.close(); } }); const q = new Request('/case/fetch-post-stream', post(body)); const s = await "
        "settle(fetch(q, { keepalive: true }).then((r) => r.status)); await tick(); return [s, pulls, body.locked, "
        "q.bodyUsed].join(' / ')",
        "value: fulfilled 200 / 2 / true / true",
    ),
    "a stream body with a Content-Type": (
        "const r = await fetch('/case/fetch-post-stream', post(stream(enc.encode('x')), { headers: { 'Content-Type': "
        "'text/plain' } })); return r.status",
        "value: 200",
    ),
    "an abort while the body streams": (
        "const ac = new AbortController(); let cancelled = 'no'; const body = new ReadableStream({ pull(c) { "
        "c.enqueue(enc.encode('x')); return new Promise(() => {}); }, cancel(reason) { cancelled = String(reason); } "
        "}); const p = fetch('/case/fetch-post-stream', post(body, { signal: ac.signal })); setTimeout(() => "
        "ac.abort(), 200); const s = await settle(p); await tick(); return [s, cancelled, body.locked].join(' / ')",
        "value: rejected AbortError: signal is aborted without reason / AbortError: signal is aborted without reason / "
        "true",
    ),
    "a 302 after a stream body": (
        "return await settle(fetch('/case/redirect-fetch-1', post(stream(enc.encode('x')))))",
        "value: rejected TypeError: Failed to fetch",
    ),
    "a stream body to another site goes after a preflight": (
        "const r = await fetch(crossSite + '/case/fetch-post-json-cross-site', post(stream(enc.encode('x')))); "
        "return [r.status, r.type].join()",
        "value: 200,cors",
    ),
    "a stream fetch of mode same-origin to another site": (
        "return await settle(fetch(crossSite + '/case/fetch-post-json-cross-site', post(stream(enc.encode('x')), "
        "{ mode: 'same-origin' })).then((r) => [r.status, r.type].join()))",
        "value: fulfilled 200,cors",
    ),
    "a keepalive fetch with a stream body": (
        "try { await fetch('/case/fetch-post-stream', post(new ReadableStream(), { keepalive: true })); return 'sent'; "
        "} catch (e) { return String(e); }",
        "value: TypeError: Failed to execute 'fetch' on 'Window': Keepalive request cannot have a ReadableStream body.",
    ),
    "a GET with a stream body": (
        "try { await fetch('/case/fetch-post-stream', { body: new ReadableStream(), duplex: 'half' }); return 'sent'; "
        "} catch (e) { return String(e); }",
        "value: TypeError: Failed to execute 'fetch' on 'Window': Request with GET/HEAD method cannot have body.",
    ),
}

# A chunk larger than what the Session and //net take of a stream, so that its rest waits for room when the request is
# aborted. It sends more than the capture server, which opens no window, lets an HTTP/2 connection carry, so it runs on a
# connection of its own.
_UPLOADS_HELD_BACK = {
    "an abort while a chunk waits for room": (
        "const ac = new AbortController(); let cancelled = 'no'; const body = new ReadableStream({ pull(c) { "
        "c.enqueue(new Uint8Array(1000000)); return new Promise(() => {}); }, cancel(reason) { cancelled = "
        "String(reason); } }); const p = fetch('/case/fetch-post-stream', post(body, { signal: ac.signal })); "
        "setTimeout(() => ac.abort('stop'), 200); const s = await settle(p); return [s, cancelled].join(' / ')",
        "value: rejected stop / stop",
    ),
}

# The same over HTTP/1.1, where Chrome sends no stream: //net refuses the request before anything goes, so the stream is
# never read.
_UPLOADS_OVER_HTTP1 = {
    "a stream body": (
        "let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; c.enqueue(enc.encode('x')); } }); const s "
        "= await settle(fetch('/case/fetch-post-stream', post(body))); await tick(); return [s, pulls, "
        "body.locked].join(' / ')",
        "value: rejected TypeError: Failed to fetch / 1 / true",
    ),
    "a keepalive fetch of a Request with a stream body": (
        "let pulls = 0; const body = new ReadableStream({ pull(c) { pulls++; c.enqueue(enc.encode('x')); if (pulls "
        "=== 2) c.close(); } }); const q = new Request('/case/fetch-post-stream', post(body)); const s = await "
        "settle(fetch(q, { keepalive: true }).then((r) => r.status)); await tick(); return [s, pulls, body.locked, "
        "q.bodyUsed].join(' / ')",
        "value: rejected TypeError: Failed to fetch / 1 / true / true",
    ),
    "an abort of a stream body": (
        "const ac = new AbortController(); let cancelled = 'no'; const body = new ReadableStream({ pull(c) { "
        "c.enqueue(enc.encode('x')); }, cancel(reason) { cancelled = String(reason); } }); const p = "
        "fetch('/case/fetch-post-stream', post(body, { signal: ac.signal })); ac.abort('stop'); return [await "
        "settle(p), cancelled].join(' / ')",
        "value: rejected stop / stop",
    ),
}

# What the Recording Case's page runs, but for where its URL comes from.
_CASE_SCRIPT = """(async () => {
  const streamBody = () => new ReadableStream({
    start(controller) {
      controller.enqueue(new TextEncoder().encode("stream body"));
      controller.close();
    },
  });
  const response = await fetch("/case/fetch-post-stream", {method: "POST", duplex: "half", body: streamBody()});
  return response.status;
})()"""


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def capture() -> Iterator[CaptureServer]:
    with CaptureServer() as server:
        yield server


def _method(request: Http1Request | Http2Request | Http3Request) -> str:
    if isinstance(request, Http1Request):
        return request.method
    return next(value for name, value in request.headers if name == ":method")


def _run(capture: CaptureServer, protocol: str, scripts: dict[str, tuple[str, str]]) -> dict[str, str]:
    """Run each script in a Page at the capture server over a protocol, and return what each gave."""
    run = run_of(_CASE, protocol)
    assert run is not None
    url = capture.url(run.route, host=run.host, path=_PAGE_PATH)
    with (
        mitto.Session(_settings=session_settings(capture, run)) as session,
        session._open_page(url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        outcomes = {
            name: page.evaluate(
                f"(async () => {{ {_UPLOAD_PRELUDE} try {{ const value = await (async () => {{ {script} }})();"
                " return 'value: ' + value; } catch (error) { return 'threw: ' + error; } })()",
                timeout=_TIMEOUT_SECONDS,
            )
            for name, (script, _) in scripts.items()
        }
        assert page.uncaught_errors == []
    return outcomes


def _requests(capture: CaptureServer, protocol: str) -> set[str]:
    """Return the method and target of each request that reached the capture server over a protocol, at either site."""
    run = run_of(_CASE, protocol)
    assert run is not None
    return {
        f"{_method(request)} {request.target}"
        for host in (run.host, CROSS_SITE_HOST)
        for connection in capture.recording(run.route, host=host).connections
        for request in connection.requests
    }


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_a_stream_body_goes_as_in_chrome(capture: CaptureServer, protocol: str) -> None:
    assert _run(capture, protocol, _UPLOADS) == {name: chrome for name, (_, chrome) in _UPLOADS.items()}
    requests = _requests(capture, protocol)
    # A stream's request to another origin goes after a CORS preflight, even with a CORS-safelisted method and no
    # header lines, as Blink's use-CORS-preflight flag has it, and so does one of mode same-origin.
    assert "OPTIONS /case/fetch-post-json-cross-site" in requests
    assert "POST /case/fetch-post-json-cross-site" in requests


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_an_abort_gives_a_stream_its_reason_while_a_chunk_waits_as_in_chrome(
    capture: CaptureServer, protocol: str
) -> None:
    assert _run(capture, protocol, _UPLOADS_HELD_BACK) == {
        name: chrome for name, (_, chrome) in _UPLOADS_HELD_BACK.items()
    }


@pytest.mark.usefixtures("page_program")
def test_a_stream_body_fails_over_http1_with_nothing_sent_as_in_chrome(capture: CaptureServer) -> None:
    assert _run(capture, HTTP1, _UPLOADS_OVER_HTTP1) == {
        name: chrome for name, (_, chrome) in _UPLOADS_OVER_HTTP1.items()
    }
    # The favicon comes after the document's load, before or after the test looks.
    assert _requests(capture, HTTP1) - {f"GET {FAVICON_PATH}"} == {f"GET {_PAGE_PATH}"}


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("protocol", [HTTP2, HTTP3])
def test_the_recording_cases_stream_goes_as_its_baseline_says(capture: CaptureServer, protocol: str) -> None:
    run = run_of(_CASE, protocol)
    assert run is not None
    case = next(case for case in CASES if case.name == _CASE)
    url = capture.url(run.route, host=run.host, path=_PAGE_PATH)
    with (
        mitto.Session(_settings=session_settings(capture, run)) as session,
        session._open_page(url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(_CASE_SCRIPT, timeout=_TIMEOUT_SECONDS) == 200
    sent = cast(dict[str, Any], normal_form(capture.recording(run.route, host=run.host), [case])["requests"])[_CASE]

    release = load_persona(ENGINE_RELEASE_PERSONA).release
    baseline = json.loads((BASELINES_DIRECTORY / release / f"{run.name}.json").read_text(encoding="utf-8"))
    chrome = baseline["requests"][_CASE]
    # Chrome's page was the case's start page, and this one is another of the same origin.
    referrer = "referer: <origin>/case/start"
    assert referrer in chrome["headers"]
    assert sent == {
        **chrome,
        "headers": [f"referer: <origin>{_PAGE_PATH}" if line == referrer else line for line in chrome["headers"]],
    }
