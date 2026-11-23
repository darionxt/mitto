"""Tests of cross-origin isolation in Pages that a Session opened, on a local server.

A page that Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy isolate in Chrome is isolated in a Page, and one
that Chrome does not isolate is not: what each page sees is held to what Chrome 157 gave for the same pages
(docs/runtime.md, "Cross-origin isolation").
"""

from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._native import PrivateSettings
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer

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


# Whether a reading in milliseconds is a whole number of steps, within half a microsecond: Blink subtracts two
# uptime-sized doubles for a reading, and performance.timeOrigin is a double near 1.7e12, whose last place is a quarter
# of a microsecond, while readings on two grids differ by 5 microseconds at least.
_WHOLE = "const whole = (ms, step) => Math.abs(ms - Math.round(ms / step) * step) < 5e-4;"

# What a page reads of its clocks, by rule rather than by value. For each clock, many readings: whether each is a whole
# number of steps of 5 microseconds, and whether some is not one of 100, which a clock of 5 microseconds gives among
# so many readings and one of 100 never does. The last item is whether performance.timeOrigin, a single reading, is a
# whole number of steps of 100.
_CLOCKS = (
    "(async () => {"
    + _WHOLE
    + """
    const fine = (readings) => readings.every((t) => whole(t, 0.005));
    const offCoarse = (readings) => readings.some((t) => !whole(t, 0.1));
    const now = [];
    const end = performance.now() + 30;
    while (performance.now() < end) {
        now.push(performance.now());
    }
    const temporal = [];
    for (let i = 0; i < 2000; i++) {
        temporal.push(Temporal.Now.instant().epochNanoseconds);
    }
    const events = [];
    const eventsEnd = performance.now() + 5;
    while (performance.now() < eventsEnd) {
        events.push(new Event('x').timeStamp);
    }
    const frames = [];
    for (let i = 0; i < 20; i++) {
        frames.push(await new Promise((resolve) => requestAnimationFrame(resolve)));
    }
    const idle = await new Promise((resolve) => requestIdleCallback((deadline) => {
        resolve(Array.from({length: 2000}, () => deadline.timeRemaining()));
    }));
    return [
        fine(now), offCoarse(now),
        temporal.every((t) => t % 5000n === 0n), temporal.some((t) => t % 100000n !== 0n),
        fine(events), offCoarse(events),
        fine(frames), offCoarse(frames),
        fine(idle), offCoarse(idle),
        whole(performance.timeOrigin, 0.005),
        typeof performance.measureUserAgentSpecificMemory,
        whole(performance.timeOrigin, 0.1),
    ];
})()"""
)


@pytest.fixture
def sixty_hertz_session(page_program: None) -> Iterator[mitto.Session]:
    """A Session whose display runs at 60 Hz, whose vsyncs 16,667 microseconds apart walk across the grid of 100: at
    100 Hz, of 10,000, every frame could fall on it, as the first one did."""
    with mitto.Session(machine=mitto.Machine.draw(refresh_rate=60)) as session:
        yield session


def test_an_isolated_window_reads_clocks_of_5_microseconds(
    sixty_hertz_session: mitto.Session, server: RecordingServer
) -> None:
    session = sixty_hertz_session
    url = server.page("/", _BLANK, _ISOLATING)

    # Whether one reading of the time origin falls on the coarse grid is chance: the workers' test reads several.
    assert _evaluate(session, url, _CLOCKS)[:-1] == [True] * 11 + ["function"]


def test_a_window_that_is_not_isolated_reads_clocks_of_100_microseconds(
    sixty_hertz_session: mitto.Session, server: RecordingServer
) -> None:
    session = sixty_hertz_session
    url = server.page("/", _BLANK)

    assert _evaluate(session, url, _CLOCKS) == [True, False] * 5 + [True, "undefined", True]


# Eight workers each read performance.now() for 30 ms and give the rules of their readings, each worker's own, and
# their time origins, of which some is off the coarse grid where they are isolated.
_WORKERS_CLOCKS = (
    "(() => {"
    + _WHOLE
    + """
    return Promise.all(Array.from({length: 8}, () => new Promise((resolve) => {
        new Worker('/clock.js').onmessage = (e) => resolve(e.data);
    }))).then((results) => [
        results.map(([fine]) => fine),
        results.map(([, offCoarse]) => offCoarse),
        results.every(([, , origin]) => whole(origin, 0.005)),
        results.some(([, , origin]) => !whole(origin, 0.1)),
        results.every(([, , , measure]) => measure === 'undefined'),
    ]);
})()"""
)

_CLOCK_WORKER = (
    _WHOLE.encode() + b"const now = []; const end = performance.now() + 30;"
    b" while (performance.now() < end) { now.push(performance.now()); }"
    b"postMessage([now.every((t) => whole(t, 0.005)), now.some((t) => !whole(t, 0.1)), performance.timeOrigin,"
    b" typeof performance.measureUserAgentSpecificMemory]);"
)


def test_an_isolated_worker_reads_clocks_of_5_microseconds(session: mitto.Session, server: RecordingServer) -> None:
    url = server.page("/", _BLANK, _ISOLATING)
    server.route("/clock.js", _CLOCK_WORKER, _JS, [_COEP])

    # measureUserAgentSpecificMemory() is exposed on a window, a service worker and a shared worker alone.
    assert _evaluate(session, url, _WORKERS_CLOCKS) == [[True] * 8, [True] * 8, True, True, True]


def test_a_worker_that_is_not_isolated_reads_clocks_of_100_microseconds(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", _BLANK)
    server.route("/clock.js", _CLOCK_WORKER, _JS)

    assert _evaluate(session, url, _WORKERS_CLOCKS) == [[True] * 8, [False] * 8, True, False, True]
