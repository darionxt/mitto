"""Tests of cross-origin isolation in Pages that a Session opened, on a local server.

A page that Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy isolate in Chrome is isolated in a Page, and one
that Chrome does not isolate is not: what each page sees is held to what Chrome 157 gave for the same pages
(docs/runtime.md, "Cross-origin isolation").
"""

import json
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._native import PrivateSettings
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, Route

_Header = tuple[str, str]
_COOP: _Header = ("Cross-Origin-Opener-Policy", "same-origin")
_COEP: _Header = ("Cross-Origin-Embedder-Policy", "require-corp")
_ISOLATING: list[_Header] = [_COOP, _COEP]
_BLANK = "<!doctype html><title>i</title>"
# What a page reads of its isolation.
_ISOLATION = "[crossOriginIsolated, typeof SharedArrayBuffer, typeof Atomics.waitAsync]"
_ISOLATED = [True, "function", "function"]
_NOT_ISOLATED = [False, "undefined", "function"]
# A name that maps to the server, at which no origin is potentially trustworthy.
_UNTRUSTWORTHY_HOST = "untrustworthy.test"
_COOP_IGNORED = (
    "The Cross-Origin-Opener-Policy header has been ignored, because the URL's origin was untrustworthy. It was defined"
    " either in the final response or a redirect. Please deliver the response using the HTTPS protocol. You can also"
    " use the 'localhost' origin instead. See https://www.w3.org/TR/powerful-features/#potentially-trustworthy-origin"
    " and https://html.spec.whatwg.org/#the-cross-origin-opener-policy-header."
)
_JS = "text/javascript"
# Worker scripts, each with the embedder policy that lets it run in an isolated page.
_WORKERS = {
    "/report.js": b"postMessage([crossOriginIsolated, typeof SharedArrayBuffer]);",
    "/add.js": b"onmessage = (e) => { const a = new Int32Array(e.data); Atomics.add(a, 0, 5); postMessage('added'); };",
    "/notify.js": b"onmessage = (e) => { const a = new Int32Array(e.data); Atomics.store(a, 0, 1); Atomics.notify(a, 0); };",
    "/nested.js": b"const inner = new Worker('/report.js'); inner.onmessage = (e) => postMessage(e.data);",
    "/broadcast.js": b"const channel = new BroadcastChannel('b');"
    b"channel.onmessage = (e) => postMessage(['message', Object.prototype.toString.call(e.data)]);"
    b"channel.onmessageerror = () => postMessage(['messageerror']); postMessage('ready');",
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip unless a Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    """A server on 127.0.0.1, whose origin is potentially trustworthy, with the worker scripts."""
    with RecordingServer() as page_server:
        for path, body in _WORKERS.items():
            page_server.route(path, body, _JS, [_COEP])
        page_server.route("/plain.js", b"postMessage('ran');", _JS)
        yield page_server


@pytest.fixture
def session(page_program: None) -> Iterator[mitto.Session]:
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _evaluate(session: mitto.Session, url: str, script: str) -> Any:
    with _open_page(session, url) as page:
        return page.evaluate(script, timeout=TIMEOUT_SECONDS)


def test_coop_same_origin_with_coep_require_corp_isolates_the_page(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)

    assert _evaluate(session, url, _ISOLATION) == _ISOLATED


def test_coep_credentialless_isolates_the_page_too(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", _BLANK, [_COOP, ("Cross-Origin-Embedder-Policy", "credentialless")])

    assert _evaluate(session, url, _ISOLATION) == _ISOLATED


@pytest.mark.parametrize(
    "headers",
    [
        pytest.param([], id="neither"),
        pytest.param([_COOP], id="coop-alone"),
        pytest.param([_COEP], id="coep-alone"),
        pytest.param([("Cross-Origin-Opener-Policy", "same-origin-allow-popups"), _COEP], id="allow-popups"),
        pytest.param([("Cross-Origin-Opener-Policy", "noopener-allow-popups"), _COEP], id="noopener-allow-popups"),
        pytest.param([_COOP, ("Cross-Origin-Embedder-Policy", "require-corp, require-corp")], id="coep-twice"),
        pytest.param([_COOP, ("Cross-Origin-Embedder-Policy", '"require-corp"')], id="coep-string"),
        pytest.param(
            [
                ("Cross-Origin-Opener-Policy-Report-Only", "same-origin"),
                ("Cross-Origin-Embedder-Policy-Report-Only", "require-corp"),
            ],
            id="report-only",
        ),
    ],
)
def test_a_page_that_chrome_does_not_isolate_is_not_isolated(
    session: mitto.Session, server: RecordingServer, headers: list[_Header]
) -> None:
    url = server.page("/", _BLANK, headers)

    assert _evaluate(session, url, _ISOLATION) == _NOT_ISOLATED


def test_coop_at_an_untrustworthy_origin_is_ignored_with_chromes_console_error(
    page_program: None, server: RecordingServer
) -> None:
    server.page("/", _BLANK, _ISOLATING)
    url = f"http://{_UNTRUSTWORTHY_HOST}:{server.origin.rsplit(':', 1)[1]}/"

    with (
        mitto.Session(_settings=PrivateSettings(host_resolver_rules=f"MAP {_UNTRUSTWORTHY_HOST} 127.0.0.1")) as session,
        _open_page(session, url) as page,
    ):
        assert page.evaluate(_ISOLATION, timeout=TIMEOUT_SECONDS) == _NOT_ISOLATED
        assert [(message.level, message.text) for message in page.console_messages] == [("error", _COOP_IGNORED)]


def test_a_page_that_navigates_to_a_page_that_is_not_isolated_is_no_longer_isolated(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    server.page("/next", _BLANK)

    with _open_page(session, url) as page:
        before = page.evaluate(_ISOLATION, timeout=TIMEOUT_SECONDS)
        page.evaluate("location.href = '/next'", timeout=TIMEOUT_SECONDS)
        page.wait_for_navigation(timeout=TIMEOUT_SECONDS)
        after = page.evaluate(_ISOLATION, timeout=TIMEOUT_SECONDS)

    assert [before, after] == [_ISOLATED, _NOT_ISOLATED]


def test_a_worker_of_an_isolated_page_is_isolated_but_one_of_a_data_url_is_not(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    script = """Promise.all(['/report.js', '/nested.js', 'data:text/javascript,' + encodeURIComponent(
        'postMessage([crossOriginIsolated, typeof SharedArrayBuffer]);')].map((src) => new Promise((resolve) => {
        new Worker(src).onmessage = (e) => resolve(e.data);
    })))"""

    assert _evaluate(session, url, script) == [[True, "function"], [True, "function"], [False, "undefined"]]


def test_a_worker_whose_script_has_no_embedder_policy_fails_in_an_isolated_page(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    script = """new Promise((resolve) => {
        const worker = new Worker('/plain.js');
        worker.onmessage = (e) => resolve(e.data);
        worker.onerror = (e) => resolve([e.constructor.name, e.type]);
    })"""

    assert _evaluate(session, url, script) == ["Event", "error"]


def test_a_shared_array_buffer_shares_its_memory_with_a_worker(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    script = """new Promise((resolve) => {
        const memory = new SharedArrayBuffer(4);
        new Int32Array(memory)[0] = 2;
        const worker = new Worker('/add.js');
        worker.onmessage = () => resolve(new Int32Array(memory)[0]);
        worker.postMessage(memory);
    })"""

    assert _evaluate(session, url, script) == 7


def test_a_shared_array_buffer_posted_to_the_window_is_a_new_object_on_the_same_memory(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    script = """new Promise((resolve) => {
        const memory = new SharedArrayBuffer(4);
        onmessage = (e) => {
            new Int32Array(e.data[0])[0] = 9;
            resolve([e.data[0] === memory, e.data[0] === e.data[1], new Int32Array(memory)[0]]);
        };
        postMessage([memory, memory], '*');
    })"""

    assert _evaluate(session, url, script) == [False, True, 9]


def test_atomics_wait_async_times_out_and_wakes_on_a_workers_notify(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    script = """(async () => {
        const memory = new Int32Array(new SharedArrayBuffer(4));
        const timedOut = await Atomics.waitAsync(memory, 0, 0, 10).value;
        const waiting = Atomics.waitAsync(memory, 0, 0);
        new Worker('/notify.js').postMessage(memory.buffer);
        return [timedOut, waiting.async, await waiting.value, Atomics.load(memory, 0)];
    })()"""

    assert _evaluate(session, url, script) == ["timed-out", True, "ok", 1]


def test_a_shared_array_buffer_is_still_refused_where_the_page_is_not_isolated(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK)
    script = """(() => {
        const memory = new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer;
        try {
            postMessage(memory, '*');
        } catch (e) {
            return [e.name, e.message];
        }
    })()"""

    assert _evaluate(session, url, script) == [
        "DataCloneError",
        "Failed to execute 'postMessage' on 'Window': SharedArrayBuffer transfer requires self.crossOriginIsolated.",
    ]


def test_a_worker_of_a_data_url_shares_the_memory_that_its_creator_posts_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    # A worker of a data: URL is not isolated, but it is of its creator's agent cluster, so Chrome hands it the buffer,
    # directly and through a port.
    script = """Promise.all([false, true].map((through_port) => new Promise((resolve) => {
        const memory = new SharedArrayBuffer(4);
        const worker = new Worker('data:text/javascript,' + encodeURIComponent(`
            const take = (e) => {
                new Int32Array(e.data)[0] = 5;
                postMessage([Object.prototype.toString.call(e.data), crossOriginIsolated]);
            };
            onmessage = (e) => {
                if (e.ports.length) {
                    e.ports[0].onmessage = take;
                    postMessage('ready');
                } else {
                    take(e);
                }
            };`));
        const channel = new MessageChannel();
        worker.onmessage = (e) => {
            if (e.data === 'ready') {
                channel.port1.postMessage(memory);
            } else {
                resolve([...e.data, new Int32Array(memory)[0]]);
            }
        };
        if (through_port) {
            worker.postMessage(null, [channel.port2]);
        } else {
            worker.postMessage(memory);
        }
    })))"""

    assert _evaluate(session, url, script) == [["[object SharedArrayBuffer]", False, 5]] * 2


def test_a_shared_array_buffer_that_a_broadcast_channel_carries_arrives_as_null(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    # Chrome's channels carry a message through the browser, which takes no shared memory along.
    script = """Promise.all([
        new Promise((resolve) => {
            const sender = new BroadcastChannel('a');
            const receiver = new BroadcastChannel('a');
            receiver.onmessage = (e) => resolve(['message', Object.prototype.toString.call(e.data)]);
            receiver.onmessageerror = () => resolve(['messageerror']);
            sender.postMessage(new SharedArrayBuffer(4));
        }),
        new Promise((resolve) => {
            const worker = new Worker('/broadcast.js');
            const sender = new BroadcastChannel('b');
            worker.onmessage = (e) => {
                if (e.data === 'ready') {
                    sender.postMessage(new SharedArrayBuffer(4));
                } else {
                    resolve(e.data);
                }
            };
        }),
    ])"""

    assert _evaluate(session, url, script) == [["message", "[object Null]"]] * 2


# A frame's document that asks for /seen/, its host and its own path once it runs, so that whether a frame of another
# origin, which the page cannot read, committed its response shows on the server.
_FRAME = b"<!doctype html><title>f</title><script>fetch('/seen/' + location.hostname + location.pathname);</script>"
_CREDENTIALLESS: _Header = ("Cross-Origin-Embedder-Policy", "credentialless")
_FRAMES: dict[str, list[_Header]] = {
    "/f-none": [],
    "/f-coep": [_COEP],
    "/f-cl": [_CREDENTIALLESS],
    "/f-coep-corp": [_COEP, ("Cross-Origin-Resource-Policy", "cross-origin")],
}
# For each frame's URL, the page reads whether its document is the page's to read, and then whether it is isolated
# and has SharedArrayBuffer. Another origin's never is, and neither is Chrome's error page.
_FRAMES_SCRIPT = """(async () => {
    const one = (src) => new Promise((resolve) => {
        const frame = document.createElement('iframe');
        frame.onload = () => resolve(frame.contentDocument
            ? [true, frame.contentWindow.crossOriginIsolated, typeof frame.contentWindow.SharedArrayBuffer]
            : [false]);
        frame.src = src;
        document.documentElement.appendChild(frame);
    });
    const out = {};
    for (const src of SOURCES) {
        out[src] = await one(src);
    }
    return out;
})()"""


@pytest.mark.parametrize(
    "parent", [[_COOP, _COEP], [_COOP, _CREDENTIALLESS], []], ids=["require-corp", "credentialless", "plain"]
)
def test_a_frame_navigation_is_held_to_its_parent_s_embedder_policy(
    session: mitto.Session, server: RecordingServer, parent: list[_Header]
) -> None:
    # NavigationRequest::CheckResponseAdherenceToCoep() and EnforceCOEP(): under require-corp or credentialless, a
    # frame's response needs a compatible embedder policy, and one of another origin also needs
    # Cross-Origin-Resource-Policy, else the frame commits Chrome's error page. A same-origin frame of an isolated
    # page is isolated; one of another origin is not (Chrome for Testing 157, localhost being another origin here).
    for path, headers in _FRAMES.items():
        server.route(path, _FRAME, "text/html", headers)
    other = server.origin.replace("127.0.0.1", "localhost")
    sources = [
        "/f-none",
        "/f-coep",
        "/f-cl",
        "/f-coep-corp",
        f"{other}/f-coep",
        f"{other}/f-coep-corp",
        f"{other}/f-none",
    ]
    url = server.page("/", _BLANK, parent)
    isolated = bool(parent)
    # The frames whose documents run, by host and path.
    committed = {"/seen/localhost/f-coep-corp", "/seen/127.0.0.1/f-coep", "/seen/127.0.0.1/f-cl"}
    committed |= {"/seen/127.0.0.1/f-coep-corp"}
    if not isolated:
        committed |= {"/seen/127.0.0.1/f-none", "/seen/localhost/f-coep", "/seen/localhost/f-none"}

    with _open_page(session, url) as page:
        frames = page.evaluate(_FRAMES_SCRIPT.replace("SOURCES", json.dumps(sources)), timeout=TIMEOUT_SECONDS)
        assert all(server.wait_for_path(path) for path in committed), server.paths()

    same_origin = [True, isolated, "function" if isolated else "undefined"]
    blocked = [False]
    assert frames == {
        "/f-none": blocked if isolated else same_origin,
        "/f-coep": same_origin,
        "/f-cl": same_origin,
        "/f-coep-corp": same_origin,
        f"{other}/f-coep": blocked,
        f"{other}/f-coep-corp": blocked,
        f"{other}/f-none": blocked,
    }
    assert {path for path in server.paths() if path.startswith("/seen/")} == committed


def test_frames_made_without_the_network_take_their_parent_s_isolation(
    session: mitto.Session, server: RecordingServer
) -> None:
    # A frame's initial about:blank, a frame nested in it and a srcdoc have their parent's origin, and so its
    # isolation, as the cross-origin-isolated feature's default allowlist 'self' has it (Chrome for Testing 157).
    script = """(async () => {
        const blank = document.createElement('iframe');
        document.documentElement.appendChild(blank);
        const nested = blank.contentDocument.createElement('iframe');
        blank.contentDocument.documentElement.appendChild(nested);
        const srcdoc = document.createElement('iframe');
        srcdoc.srcdoc = '<p>x';
        document.documentElement.appendChild(srcdoc);
        await new Promise((resolve) => srcdoc.onload = resolve);
        return [crossOriginIsolated, blank.contentWindow.crossOriginIsolated, typeof blank.contentWindow.SharedArrayBuffer,
                nested.contentWindow.crossOriginIsolated, srcdoc.contentWindow.crossOriginIsolated];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [True, True, "function", True, True]
    assert _evaluate(session, server.page("/plain", _BLANK), script) == [False, False, "undefined", False, False]


def test_a_frame_s_requests_are_held_to_its_own_embedder_policy(
    session: mitto.Session, server: RecordingServer
) -> None:
    # A frame's document has its response's embedder policy, so its no-cors fetch() of another origin without
    # Cross-Origin-Resource-Policy fails under require-corp (Chrome for Testing 157).
    server.route(
        "/f-fetch",
        b"<!doctype html><title>f</title><script>window.fetched = fetch('http://localhost:' + location.port +"
        b" '/no-corp', {mode: 'no-cors'}).then(() => 'ok', (e) => e.name);</script>",
        "text/html",
        [_COEP],
    )
    server.route("/no-corp", b"x", "text/plain")
    script = """(async () => {
        const frame = document.createElement('iframe');
        frame.src = '/f-fetch';
        document.documentElement.appendChild(frame);
        await new Promise((resolve) => frame.onload = resolve);
        return await frame.contentWindow.fetched;
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == "TypeError"


def test_a_sandboxed_frame_is_isolated_only_where_it_keeps_its_origin(
    session: mitto.Session, server: RecordingServer
) -> None:
    # A sandbox with allow-same-origin keeps a frame's origin and so its isolation (Chrome for Testing 157). Without it
    # Chrome's frame has an opaque origin, which the default allowlist does not take: the Page does not make that
    # origin yet (#285), but gives such a frame no isolation either.
    script = """(() => {
        const frame = (sandbox) => {
            const element = document.createElement('iframe');
            element.setAttribute('sandbox', sandbox);
            document.documentElement.appendChild(element);
            return element.contentWindow;
        };
        return [frame('allow-same-origin').crossOriginIsolated,
                frame('allow-scripts allow-same-origin').crossOriginIsolated,
                frame('allow-scripts').crossOriginIsolated];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [True, True, False]


def test_a_frame_response_that_is_sniffed_is_held_to_its_parent_s_embedder_policy(
    session: mitto.Session, server: RecordingServer
) -> None:
    # A response with no Content-Type is sniffed before it is known to make a document, and only then held to the
    # parent's policy: Chrome's frame commits its error page, and the Page goes on (Chrome for Testing 157).
    server.routes["/sniffed"] = Route(b"<!doctype html><title>s</title><p>sniffed", headers=[])
    script = """(async () => {
        const frame = document.createElement('iframe');
        frame.src = '/sniffed';
        document.documentElement.appendChild(frame);
        await new Promise((resolve) => frame.onload = resolve);
        return [frame.contentDocument === null, crossOriginIsolated];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [True, True]


def test_a_frame_whose_sandbox_goes_takes_its_parent_s_isolation_again(
    session: mitto.Session, server: RecordingServer
) -> None:
    # A frame made under a sandbox without allow-same-origin is not isolated; once the sandbox goes, the srcdoc that it
    # then commits is, in a window of its own (Chrome for Testing 157).
    script = """(async () => {
        const frame = document.createElement('iframe');
        frame.setAttribute('sandbox', 'allow-scripts');
        document.documentElement.appendChild(frame);
        frame.removeAttribute('sandbox');
        frame.srcdoc = '<p>x';
        await new Promise((resolve) => frame.onload = resolve);
        return [frame.contentWindow.crossOriginIsolated, typeof frame.contentWindow.SharedArrayBuffer];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [True, "function"]


def test_a_frame_whose_response_sandboxes_it_is_not_isolated(session: mitto.Session, server: RecordingServer) -> None:
    # Content-Security-Policy: sandbox without allow-same-origin gives Chrome's frame document an opaque origin, which
    # the parent cannot read (Chrome for Testing 157). The Page does not make that origin yet, but gives the frame no
    # isolation either. A frame element's sandbox attribute means nothing, so that frame keeps its isolation.
    server.route(
        "/f-csp",
        b"<!doctype html><title>c</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox allow-scripts")],
    )
    script = """(async () => {
        const frame = document.createElement('iframe');
        frame.src = '/f-csp';
        document.documentElement.appendChild(frame);
        await new Promise((resolve) => frame.onload = resolve);
        const legacy = document.createElement('frame');
        legacy.setAttribute('sandbox', 'allow-scripts');
        document.documentElement.appendChild(legacy);
        return [frame.contentWindow.crossOriginIsolated, legacy.contentWindow.crossOriginIsolated];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [False, True]


def test_a_frame_s_policy_is_split_as_the_parser_splits_it(session: mitto.Session, server: RecordingServer) -> None:
    # The parser ignores a duplicate directive, so a sandbox that keeps allow-same-origin, then a bare one, keeps the
    # frame's origin and its isolation. So does a sandbox whose value has a character that is not ASCII, or a vertical
    # tab, which is no white space to the parser: it leaves the flags out. A form feed is white space, and that sandbox
    # takes the frame's origin, so its document is not isolated (Chrome for Testing 157).
    server.route(
        "/f-dup",
        b"<!doctype html><title>d</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox allow-scripts allow-same-origin; sandbox")],
    )
    server.route(
        "/f-bad",
        b"<!doctype html><title>b</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox \u00e9")],
    )
    server.route(
        "/f-vt",
        b"<!doctype html><title>v</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox allow-scripts\x0ballow-forms")],
    )
    server.route(
        "/f-vt-name",
        b"<!doctype html><title>n</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox\x0b; sandbox allow-same-origin")],
    )
    server.route(
        "/f-ff",
        b"<!doctype html><title>f</title>",
        "text/html",
        [_COEP, ("Content-Security-Policy", "sandbox\x0callow-scripts")],
    )
    script = """(async () => {
        const isolated = async (src) => {
            const frame = document.createElement('iframe');
            frame.src = src;
            document.documentElement.appendChild(frame);
            await new Promise((resolve) => frame.onload = resolve);
            return frame.contentWindow.crossOriginIsolated;
        };
        return [
            await isolated('/f-dup'),
            await isolated('/f-bad'),
            await isolated('/f-vt'),
            await isolated('/f-vt-name'),
            await isolated('/f-ff'),
        ];
    })()"""

    assert _evaluate(session, server.page("/", _BLANK, _ISOLATING), script) == [True, True, True, True, False]


def test_a_redirect_s_resource_policy_comes_before_its_recursion(
    session: mitto.Session, server: RecordingServer
) -> None:
    # NavigationRequest::OnRequestRedirected() runs EnforceCOEP() before WillRedirectRequest() looks for a URL that two
    # ancestors have: a redirect of another origin without Cross-Origin-Resource-Policy to such a URL ends with the
    # error page, not with the frame keeping its document (Chrome for Testing 157).
    other = server.origin.replace("127.0.0.1", "localhost")
    server.route(
        "/p",
        b"<!doctype html><title>p</title><script>const frame = document.createElement('iframe');"
        b"frame.src = window.parent.location.pathname === '/p' ? '" + other.encode() + b"/redir' : '/p';"
        b"document.documentElement.appendChild(frame);</script>",
        "text/html",
        [_COEP],
    )
    server.routes["/redir"] = Route(b"", 302, [("Location", server.url("/p"))])
    # The outer frame's load waits for the frames under it, the grandchild's navigation among them; then the page reads
    # whether the grandchild committed the error page, which the page cannot read into, or kept its about:blank.
    script = """(async () => {
        const frame = document.createElement('iframe');
        frame.src = '/p';
        const loaded = new Promise((resolve) => frame.onload = resolve);
        document.documentElement.appendChild(frame);
        await loaded;
        const inner = frame.contentDocument.querySelector('iframe');
        const grandchild = inner.contentDocument.querySelector('iframe');
        return grandchild.contentDocument === null ? 'error page' : 'kept';
    })()"""

    with _open_page(session, server.page("/", _BLANK, _ISOLATING)) as page:
        assert page.evaluate(script, timeout=TIMEOUT_SECONDS) == "error page"
        assert server.wait_for_path("/redir")
