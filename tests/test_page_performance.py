"""Tests of a window's Performance Timeline: User Timing's marks and measures, the navigation's entry, the entries of
the Page's own requests, and the legacy performance.timing and performance.navigation, which all read one record of how
the document loaded, as Blink's do.

The expected values are what Chrome 157 gave for the same scripts, headless on macOS (157.0.8103.51): what the
Performance Timeline gives is not of the machine, and Blink's code for it is the same on every desktop. The User Timing
case runs in the Page program that MITTO_PAGE_PROGRAM names, as tests/test_page_document.py does. The others open a Page
through a Session, on a local server of two origins, and need a native library with the Page program beside it.
"""

import gzip
import http.server
import json
import os
import subprocess
import threading
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

# Marks and measures at times far from now, so that their order is the same in any run, and the errors of each
# kind of wrong call.
_USER_TIMING = """
(() => {
  const error = (f) => { try { f(); return null; } catch (e) { return `${e.name}: ${e.message}`; } };
  const tag = (o) => Object.prototype.toString.call(o);
  const detail = {x: [1, 2], y: 'z'};
  const mark = performance.mark('a', {startTime: 100000, detail});
  const plain = performance.mark('plain', {startTime: 100100});
  const constructed = new PerformanceMark('built', {startTime: 5, detail: null});
  const result = {
    mark: [mark.name, mark.entryType, mark.startTime, mark.duration, tag(mark), Object.keys(mark.toJSON())],
    detail: [mark.detail, mark.detail === mark.detail, mark.detail === detail, plain.detail],
    constructed: [constructed.startTime, constructed.detail, performance.getEntriesByName('built').length,
                  constructed.navigationId === mark.navigationId],
    navigationId: mark.navigationId >= 100 && mark.navigationId <= 10000,
    nowMark: (() => { const before = performance.now(); const m = performance.mark('now'); const ok = m.startTime >= before; performance.clearMarks('now'); return ok; })(),
    errors: {
      negativeMark: error(() => performance.mark('n', {startTime: -1})),
      timingName: error(() => performance.mark('navigationStart')),
      constructorNegative: error(() => new PerformanceMark('n', {startTime: -1})),
      constructorTimingName: error(() => new PerformanceMark('fetchStart')),
      constructorCall: error(() => PerformanceMark('x')),
      cloneFunction: error(() => performance.mark('f', {detail: function named() {}})),
      cloneSymbol: error(() => performance.mark('f', {detail: Symbol('s')})),
      cloneWindow: error(() => performance.mark('f', {detail: window})),
      cloneDocument: error(() => performance.mark('f', {detail: [document]})),
      measureOptionsAndEnd: error(() => performance.measure('m', {start: 'a'}, 'a')),
      measureNoEnds: error(() => performance.measure('m', {detail: 1})),
      measureAllThree: error(() => performance.measure('m', {start: 0, duration: 1, end: 2})),
      measureMissingStart: error(() => performance.measure('m', 'nope')),
      measureMissingEnd: error(() => performance.measure('m', undefined, 'nope')),
      measureNegative: error(() => performance.measure('m', {start: -1})),
      measureNaN: error(() => performance.measure('m', {start: NaN})),
      measureNaNDuration: error(() => performance.measure('m', {start: 1, duration: NaN})),
      measureUnfinished: error(() => performance.measure('m', 'unloadEventStart')),
      measureClone: error(() => performance.measure('m', {start: 0, detail: () => 1})),
      markWithoutName: error(() => performance.mark()),
      typeWithoutName: error(() => performance.getEntriesByType()),
    },
  };
  const fromMark = performance.measure('m1', 'a');
  const withDuration = performance.measure('m3', {start: 1, duration: 2});
  const endOnly = performance.measure('m4', {end: 10, duration: 4});
  const withDetail = performance.measure('m5', {start: 0, detail: [1]});
  performance.mark('c', {startTime: 7});
  const toMark = performance.measure('m6', 'navigationStart', 'c');
  const mixed = performance.measure('m7', {start: 'c', end: 12.5});
  const empty = performance.measure('m8', {});
  result.measures = {
    fromMark: [fromMark.startTime, fromMark.duration > 0, fromMark.entryType, tag(fromMark), fromMark.detail],
    withDuration: [withDuration.startTime, withDuration.duration],
    endOnly: [endOnly.startTime, endOnly.duration],
    detail: [withDetail.detail, Object.keys(withDetail.toJSON())],
    toMark: [toMark.startTime, toMark.duration],
    mixed: [mixed.startTime, mixed.duration],
    empty: [empty.startTime, empty.duration > 0],
  };
  performance.mark('a', {startTime: 100200});
  result.lists = {
    marks: performance.getEntriesByType('mark').map((e) => `${e.name}@${e.startTime}`),
    measures: performance.getEntriesByType('measure').filter((e) => e.name !== 'm1' && e.name !== 'm8')
      .map((e) => `${e.name}@${e.startTime}`),
    byName: performance.getEntriesByName('a').map((e) => `${e.entryType}@${e.startTime}`),
    byNameAndType: [performance.getEntriesByName('a', 'mark').length, performance.getEntriesByName('a', 'measure').length],
    visibility: performance.getEntriesByType('visibility-state').map((e) => [e.name, e.startTime, e.duration, tag(e)]),
    longtask: performance.getEntriesByType('longtask').length,
    unknown: performance.getEntriesByType('nonsense').length,
    fresh: performance.getEntries() !== performance.getEntries(),
  };
  performance.clearMarks('a');
  result.afterClearingA = performance.getEntriesByType('mark').map((e) => e.name);
  performance.clearMarks();
  performance.clearMeasures('m1');
  result.afterClearingAll = [performance.getEntriesByType('mark').length,
                             performance.getEntriesByType('measure').map((e) => e.name)];
  performance.clearMeasures();
  result.afterClearingMeasures = performance.getEntriesByType('measure').length;
  result.timing = {
    keys: Object.keys(performance.timing.toJSON()),
    same: performance.timing === performance.timing && performance.navigation === performance.navigation,
    navigation: performance.navigation.toJSON(),
    performanceKeys: Object.keys(performance.toJSON()),
    performanceTiming: performance.toJSON().timing === performance.timing,
  };
  return result;
})()
"""

# What Chrome 157 gave for _USER_TIMING.
_USER_TIMING_IN_CHROME = {
    "mark": [
        "a",
        "mark",
        100000,
        0,
        "[object PerformanceMark]",
        ["name", "entryType", "startTime", "duration", "navigationId"],
    ],
    "detail": [{"x": [1, 2], "y": "z"}, True, False, None],
    "constructed": [5, None, 0, True],
    "navigationId": True,
    "nowMark": True,
    "errors": {
        "negativeMark": "TypeError: Failed to execute 'mark' on 'Performance': 'n' cannot have a negative start time.",
        "timingName": "SyntaxError: Failed to execute 'mark' on 'Performance': "
        "'navigationStart' is part of the PerformanceTiming interface, and "
        "cannot be used as a mark name.",
        "constructorNegative": "TypeError: Failed to construct 'PerformanceMark': 'n' cannot "
        "have a negative start time.",
        "constructorTimingName": "SyntaxError: Failed to construct 'PerformanceMark': "
        "'fetchStart' is part of the PerformanceTiming interface, and "
        "cannot be used as a mark name.",
        "constructorCall": "TypeError: Failed to construct 'PerformanceMark': Please use the "
        "'new' operator, this DOM object constructor cannot be called as a "
        "function.",
        "cloneFunction": "DataCloneError: Failed to execute 'mark' on 'Performance': function "
        "named() {} could not be cloned.",
        "cloneSymbol": "DataCloneError: Failed to execute 'mark' on 'Performance': Symbol(s) could not be cloned.",
        "cloneWindow": "DataCloneError: Failed to execute 'mark' on 'Performance': #<Window> could not be cloned.",
        "cloneDocument": "DataCloneError: Failed to execute 'mark' on 'Performance': "
        "HTMLDocument object could not be cloned.",
        "measureOptionsAndEnd": "TypeError: Failed to execute 'measure' on 'Performance': If a "
        "non-empty PerformanceMeasureOptions object was passed, "
        "|end_mark| must not be passed.",
        "measureNoEnds": "TypeError: Failed to execute 'measure' on 'Performance': If a "
        "non-empty PerformanceMeasureOptions object was passed, at least one "
        "of its 'start' or 'end' properties must be present.",
        "measureAllThree": "TypeError: Failed to execute 'measure' on 'Performance': If a "
        "non-empty PerformanceMeasureOptions object was passed, it must not "
        "have all of its 'start', 'duration', and 'end' properties defined",
        "measureMissingStart": "SyntaxError: Failed to execute 'measure' on 'Performance': The "
        "mark 'nope' does not exist.",
        "measureMissingEnd": "SyntaxError: Failed to execute 'measure' on 'Performance': The "
        "mark 'nope' does not exist.",
        "measureNegative": "TypeError: Failed to execute 'measure' on 'Performance': 'm' "
        "cannot have a negative time stamp.",
        "measureNaN": "TypeError: Failed to execute 'measure' on 'Performance': Failed to read "
        "the 'start' property from 'PerformanceMeasureOptions': The provided "
        "double value is non-finite.",
        "measureNaNDuration": "TypeError: Failed to execute 'measure' on 'Performance': Failed "
        "to read the 'duration' property from "
        "'PerformanceMeasureOptions': The provided double value is "
        "non-finite.",
        "measureUnfinished": "InvalidAccessError: Failed to execute 'measure' on "
        "'Performance': 'unloadEventStart' is empty: either the event "
        "hasn't happened yet, or it would provide cross-origin timing "
        "information.",
        "measureClone": "DataCloneError: Failed to execute 'measure' on 'Performance': () => 1 could not be cloned.",
        "markWithoutName": "TypeError: Failed to execute 'mark' on 'Performance': 1 argument "
        "required, but only 0 present.",
        "typeWithoutName": "TypeError: Failed to execute 'getEntriesByType' on 'Performance': "
        "1 argument required, but only 0 present.",
    },
    "measures": {
        "fromMark": [100000, False, "measure", "[object PerformanceMeasure]", None],
        "withDuration": [1, 2],
        "endOnly": [6, 4],
        "detail": [[1], ["name", "entryType", "startTime", "duration", "navigationId"]],
        "toMark": [0, 7],
        "mixed": [7, 5.5],
        "empty": [0, True],
    },
    "lists": {
        "marks": ["c@7", "a@100000", "plain@100100", "a@100200"],
        "measures": ["m5@0", "m6@0", "m3@1", "m4@6", "m7@7"],
        "byName": ["mark@100000", "mark@100200"],
        "byNameAndType": [2, 0],
        "visibility": [["visible", 0, 0, "[object VisibilityStateEntry]"]],
        "longtask": 0,
        "unknown": 0,
        "fresh": True,
    },
    "afterClearingA": ["c", "plain"],
    "afterClearingAll": [0, ["m5", "m6", "m8", "m3", "m4", "m7"]],
    "afterClearingMeasures": 0,
    "timing": {
        "keys": [
            "connectStart",
            "secureConnectionStart",
            "unloadEventEnd",
            "domainLookupStart",
            "domainLookupEnd",
            "responseStart",
            "connectEnd",
            "responseEnd",
            "requestStart",
            "domLoading",
            "redirectStart",
            "loadEventEnd",
            "domComplete",
            "navigationStart",
            "loadEventStart",
            "domContentLoadedEventEnd",
            "unloadEventStart",
            "redirectEnd",
            "domInteractive",
            "fetchStart",
            "domContentLoadedEventStart",
        ],
        "same": True,
        "navigation": {"type": 0, "redirectCount": 0},
        "performanceKeys": ["timeOrigin", "timing", "navigation"],
        "performanceTiming": True,
    },
}


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> Any:
    result = subprocess.run(
        [program, "-"],
        input=f"JSON.stringify({script})",
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_user_timing_gives_chrome_s_marks_measures_and_errors(program: Path) -> None:
    assert _evaluate(program, _USER_TIMING) == _USER_TIMING_IN_CHROME


# The Session tests: two origins on the loopback address, the page's and another's.

# A route's status, header lines and body.
_Route = tuple[int, list[tuple[str, str]], bytes]

_JSON = json.dumps({"hello": "world " * 40}).encode()


class _Origin:
    """A local server of one origin, which answers each path from its routes."""

    def __init__(self) -> None:
        self.routes: dict[str, Callable[[], _Route]] = {}
        origin = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                route = origin.routes.get(self.path)
                status, headers, body = route() if route else (404, [("Content-Type", "text/plain")], b"no")
                self.send_response(status)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def url(self, path: str = "") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def origins() -> Iterator[tuple[_Origin, _Origin]]:
    page, other = _Origin(), _Origin()
    yield page, other
    page.close()
    other.close()


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _html(markup: str, *headers: tuple[str, str]) -> Callable[[], _Route]:
    return lambda: (200, [("Content-Type", "text/html; charset=utf-8"), *headers], markup.encode())


# What the navigation and the requests of the page below give, after its load and five fetches: of each entry, what
# says nothing of the run, which of its times are 0, and whether the others are in order.
_NAVIGATION_CASE = """
(async (OTHER) => {
  await new Promise((r) => setTimeout(r, 50));
  const read = (u, o) => fetch(u, o).then((r) => r.text()).catch((e) => `failed ${e.name}`);
  await read('/data.json');
  await read('/redirect');
  await read(OTHER + '/y.json');
  await read(OTHER + '/z.json');
  await read('/missing');
  const times = ['redirectStart', 'redirectEnd', 'fetchStart', 'domainLookupStart', 'domainLookupEnd', 'connectStart',
    'connectEnd', 'secureConnectionStart', 'requestStart', 'responseStart', 'responseEnd', 'workerStart',
    'firstInterimResponseStart', 'finalResponseHeadersStart'];
  const pick = (e) => ({
    name: e.name.replace(location.origin, '<o>').replace(OTHER, '<x>'), entryType: e.entryType,
    initiatorType: e.initiatorType, nextHopProtocol: e.nextHopProtocol, deliveryType: e.deliveryType,
    renderBlockingStatus: e.renderBlockingStatus, responseStatus: e.responseStatus, contentType: e.contentType,
    contentEncoding: e.contentEncoding, transferSize: e.transferSize, encodedBodySize: e.encodedBodySize,
    decodedBodySize: e.decodedBodySize, serverTiming: e.serverTiming.map((s) => s.toJSON()),
    frozen: Object.isFrozen(e.serverTiming), sameArray: e.serverTiming === e.serverTiming,
    zero: times.filter((t) => e[t] === 0),
    ordered: e.fetchStart <= e.domainLookupStart && e.domainLookupStart <= e.domainLookupEnd &&
      e.domainLookupEnd <= e.connectStart && e.connectStart <= e.connectEnd && e.connectEnd <= e.requestStart &&
      e.requestStart <= e.responseStart && e.responseStart <= e.responseEnd,
    durationIsEnd: Math.abs(e.duration - (e.responseEnd - e.startTime)) < 1e-6,
    worker: [e.workerRouterEvaluationStart, e.workerCacheLookupStart, e.workerMatchedSourceType, e.workerFinalSourceType],
    json: Object.keys(e.toJSON()),
  });
  const nav = performance.getEntriesByType('navigation')[0];
  const t = performance.timing;
  const legacy = ['fetchStart', 'domainLookupStart', 'domainLookupEnd', 'connectStart', 'connectEnd', 'requestStart',
    'responseStart', 'responseEnd', 'domInteractive', 'domContentLoadedEventStart', 'domContentLoadedEventEnd',
    'domComplete', 'loadEventStart', 'loadEventEnd'];
  return {
    nav: {
      ...pick(nav), type: nav.type, redirectCount: nav.redirectCount, startTime: nav.startTime,
      unload: [nav.unloadEventStart, nav.unloadEventEnd], activationStart: nav.activationStart,
      criticalCHRestart: nav.criticalCHRestart, notRestoredReasons: nav.notRestoredReasons, confidence: nav.confidence,
      documentOrder: nav.responseEnd <= nav.domInteractive && nav.domInteractive <= nav.domContentLoadedEventStart &&
        nav.domContentLoadedEventStart <= nav.domContentLoadedEventEnd &&
        nav.domContentLoadedEventEnd <= nav.domComplete && nav.domComplete <= nav.loadEventStart &&
        nav.loadEventStart <= nav.loadEventEnd && nav.loadEventEnd > 0,
      durationIsLoadEnd: nav.duration === nav.loadEventEnd,
      sameEntry: performance.getEntriesByType('navigation')[0] === nav,
      firstInTimeline: performance.getEntries()[0] === nav,
      tag: Object.prototype.toString.call(nav),
    },
    legacyAgrees: legacy.filter((k) => !(Math.abs((t[k] - t.navigationStart) - nav[k]) <= 1)),
    legacyZero: Object.keys(t.toJSON()).filter((k) => t[k] === 0),
    loadTimes: chrome.loadTimes().commitLoadTime * 1000 === t.responseStart,
    origin: Math.abs(performance.timeOrigin - t.navigationStart) <= 1,
    resources: performance.getEntriesByType('resource').map(pick),
    types: performance.getEntries().map((e) => e.entryType),
    longtask: performance.getEntriesByType('longtask').length,
  };
})
"""


def _navigation_page(page: _Origin, other: _Origin) -> str:
    page.routes["/"] = _html(
        f"<!doctype html><title>n</title><script src=/a.js></script><script async src={other.url('/x.js')}></script>"
        "<p>Hello",
        ("Server-Timing", 'db;dur=53.2;desc="Database", cache;desc=hit, total;dur=123'),
    )
    page.routes["/a.js"] = lambda: (200, [("Content-Type", "application/javascript; charset=utf-8")], b"window.a = 1;")
    page.routes["/data.json"] = lambda: (
        200,
        [("Content-Type", "application/json"), ("Content-Encoding", "gzip"), ("Server-Timing", "app;dur=1.5")],
        gzip.compress(_JSON, mtime=0),
    )
    page.routes["/redirect"] = lambda: (302, [("Location", "/data.json")], b"")
    other.routes["/x.js"] = lambda: (200, [("Content-Type", "text/javascript"), ("Server-Timing", "x;dur=2")], b"x=1")
    other.routes["/y.json"] = lambda: (
        200,
        [
            ("Content-Type", "application/json"),
            ("Access-Control-Allow-Origin", "*"),
            ("Timing-Allow-Origin", "*"),
            ("Server-Timing", "y;dur=3"),
        ],
        b'{"y":1}',
    )
    other.routes["/z.json"] = lambda: (
        200,
        [("Content-Type", "application/json"), ("Access-Control-Allow-Origin", "*"), ("Server-Timing", "z;dur=4")],
        b'{"z":1}',
    )
    return page.url("/")


# Of an entry that passed the TAO check over a new connection or a reused one, with no redirect: the times that are 0.
_ZERO = ["redirectStart", "redirectEnd", "secureConnectionStart", "workerStart", "firstInterimResponseStart"]
# Of one that failed it: every time but those of its start and end.
_HIDDEN = [
    "redirectStart",
    "redirectEnd",
    "domainLookupStart",
    "domainLookupEnd",
    "connectStart",
    "connectEnd",
    "secureConnectionStart",
    "requestStart",
    "responseStart",
    "workerStart",
    "firstInterimResponseStart",
    "finalResponseHeadersStart",
]
_RESOURCE_JSON = [
    "name",
    "entryType",
    "startTime",
    "duration",
    "navigationId",
    "initiatorType",
    "deliveryType",
    "nextHopProtocol",
    "renderBlockingStatus",
    "contentType",
    "contentEncoding",
    "workerStart",
    "workerRouterEvaluationStart",
    "workerCacheLookupStart",
    "workerMatchedSourceType",
    "workerFinalSourceType",
    "redirectStart",
    "redirectEnd",
    "fetchStart",
    "domainLookupStart",
    "domainLookupEnd",
    "connectStart",
    "secureConnectionStart",
    "connectEnd",
    "requestStart",
    "responseStart",
    "firstInterimResponseStart",
    "finalResponseHeadersStart",
    "responseEnd",
    "transferSize",
    "encodedBodySize",
    "decodedBodySize",
    "responseStatus",
    "serverTiming",
]
_NAVIGATION_JSON = [
    *_RESOURCE_JSON,
    "unloadEventStart",
    "unloadEventEnd",
    "domInteractive",
    "domContentLoadedEventStart",
    "domContentLoadedEventEnd",
    "domComplete",
    "loadEventStart",
    "loadEventEnd",
    "type",
    "redirectCount",
    "activationStart",
    "criticalCHRestart",
    "notRestoredReasons",
    "confidence",
]


def _resource(
    name: str,
    initiator: str,
    *,
    status: int = 200,
    content_type: str = "",
    encoding: str = "",
    sizes: tuple[int, int] = (0, 0),
    blocking: bool = False,
    server_timing: list[dict[str, Any]] | None = None,
    hidden: bool = False,
    zero: list[str] | None = None,
) -> dict[str, Any]:
    """An entry as _NAVIGATION_CASE describes it: `sizes` are the encoded and decoded sizes of its body."""
    return {
        "name": name,
        "entryType": "resource",
        "initiatorType": initiator,
        "nextHopProtocol": "" if hidden else "http/1.1",
        "deliveryType": "",
        "renderBlockingStatus": "blocking" if blocking else "non-blocking",
        "responseStatus": status,
        "contentType": content_type,
        "contentEncoding": encoding,
        "transferSize": 0 if hidden else sizes[0] + 300,
        "encodedBodySize": sizes[0],
        "decodedBodySize": sizes[1],
        "serverTiming": server_timing or [],
        "frozen": True,
        "sameArray": False,
        "zero": _HIDDEN if hidden else (zero if zero is not None else _ZERO),
        "ordered": not hidden,
        "durationIsEnd": True,
        "worker": [0, 0, "", ""],
        "json": _RESOURCE_JSON,
    }


@pytest.mark.usefixtures("page_program")
def test_the_timeline_holds_the_navigation_and_the_page_s_requests_as_chrome_s(
    origins: tuple[_Origin, _Origin],
) -> None:
    page, other = origins
    url = _navigation_page(page, other)
    document = page.routes["/"]()[2]
    encoded = len(gzip.compress(_JSON, mtime=0))
    with mitto.Session() as session, _open_page(session, url) as opened:
        result = opened.evaluate(f"({_NAVIGATION_CASE})({json.dumps(other.url())})")
        console = [message.text for message in opened.console_messages]

    navigation = result.pop("nav")
    confidence = navigation.pop("confidence")
    # The browser answers at random with Chrome's rate, once the document is interactive.
    assert confidence["randomizedTriggerRate"] == 0.4994798
    assert confidence["value"] in ("high", "low")
    assert navigation == {
        **_resource("<o>/", "navigation", content_type="text/html", sizes=(len(document), len(document))),
        "entryType": "navigation",
        "serverTiming": [
            {"name": "db", "duration": 53.2, "description": "Database"},
            {"name": "cache", "duration": 0, "description": "hit"},
            {"name": "total", "duration": 123, "description": ""},
        ],
        "durationIsEnd": False,
        "json": _NAVIGATION_JSON,
        "type": "navigate",
        "redirectCount": 0,
        "startTime": 0,
        "unload": [0, 0],
        "activationStart": 0,
        "criticalCHRestart": 0,
        "notRestoredReasons": None,
        "documentOrder": True,
        "durationIsLoadEnd": True,
        "sameEntry": True,
        "firstInTimeline": True,
        "tag": "[object PerformanceNavigationTiming]",
    }
    assert result == {
        # performance.timing, chrome.loadTimes() and timeOrigin read what the navigation's entry does.
        "legacyAgrees": [],
        "legacyZero": ["secureConnectionStart", "unloadEventEnd", "redirectStart", "unloadEventStart", "redirectEnd"],
        "loadTimes": True,
        "origin": True,
        "resources": [
            _resource("<o>/a.js", "script", content_type="text/javascript", sizes=(13, 13), blocking=True),
            # A script of another origin is opaque, and fails the TAO check.
            _resource("<x>/x.js", "script", status=0, hidden=True),
            _resource(
                "<o>/data.json",
                "fetch",
                content_type="application/json",
                encoding="gzip",
                sizes=(encoded, len(_JSON)),
                server_timing=[{"name": "app", "duration": 1.5, "description": ""}],
            ),
            _resource(
                "<o>/redirect",
                "fetch",
                content_type="application/json",
                encoding="gzip",
                sizes=(encoded, len(_JSON)),
                server_timing=[{"name": "app", "duration": 1.5, "description": ""}],
                zero=["secureConnectionStart", "workerStart", "firstInterimResponseStart"],
            ),
            _resource(
                "<x>/y.json",
                "fetch",
                content_type="application/json",
                sizes=(7, 7),
                server_timing=[{"name": "y", "duration": 3, "description": ""}],
            ),
            # CORS lets the page see its status and type; without Timing-Allow-Origin, nothing else.
            _resource("<x>/z.json", "fetch", content_type="application/json", hidden=True),
            _resource("<o>/missing", "fetch", status=404, content_type="text/plain", sizes=(2, 2)),
        ],
        "types": ["navigation", "visibility-state", *["resource"] * 7],
        "longtask": 0,
    }
    assert console == ["Deprecated API for given entry type."]


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize(
    ("path", "expected"),
    [
        pytest.param("/same", [1, True, True, True, [True, True, 1]], id="same-origin"),
        pytest.param("/twice", [2, True, True, True, [True, True, 2]], id="two-same-origin"),
        # A navigation with no referrer shows nothing of a redirect that went to another origin.
        pytest.param("/cross", [0, False, False, True, [False, False, 0]], id="cross-origin"),
    ],
)
def test_the_navigation_s_redirects_show_unless_one_went_to_another_origin(
    origins: tuple[_Origin, _Origin], path: str, expected: list[Any]
) -> None:
    page, other = origins
    page.routes["/same"] = lambda: (302, [("Location", "/page")], b"")
    page.routes["/twice"] = lambda: (302, [("Location", "/same")], b"")
    page.routes["/cross"] = lambda: (302, [("Location", other.url("/page"))], b"")
    page.routes["/page"] = other.routes["/page"] = _html("<p>landed")
    with mitto.Session() as session, _open_page(session, page.url(path)) as opened:
        result = opened.evaluate(
            """(() => {
              const n = performance.getEntriesByType('navigation')[0];
              const t = performance.timing;
              return [n.redirectCount, n.redirectStart > 0, n.redirectEnd > 0,
                      n.redirectStart <= n.redirectEnd && n.redirectEnd <= n.fetchStart,
                      [t.redirectStart > 0, t.redirectEnd > 0, performance.navigation.redirectCount]];
            })()"""
        )

    assert result == expected


@pytest.mark.usefixtures("page_program")
def test_a_full_resource_buffer_fires_its_event_and_keeps_what_the_handler_makes_room_for(
    origins: tuple[_Origin, _Origin],
) -> None:
    page, _ = origins
    page.routes["/"] = _html("<p>buffer")
    for name in ("/one", "/two", "/three"):
        page.routes[name] = lambda: (200, [("Content-Type", "text/plain")], b"x")
    with mitto.Session() as session, _open_page(session, page.url("/")) as opened:
        result = opened.evaluate(
            """(async () => {
              const events = [];
              performance.onresourcetimingbufferfull = (e) => {
                events.push([e.type, e.isTrusted, e.bubbles, e.target === performance]);
                if (events.length === 1) performance.setResourceTimingBufferSize(2);
              };
              performance.setResourceTimingBufferSize(1);
              for (const path of ['/one', '/two', '/three']) await fetch(path).then((r) => r.text());
              await new Promise((r) => setTimeout(r, 50));
              const names = performance.getEntriesByType('resource').map((e) => e.name.slice(e.name.lastIndexOf('/')));
              performance.clearResourceTimings();
              return [events, names, performance.getEntriesByType('resource').length];
            })()"""
        )

    # The first event makes room for one more; the third entry then has none, and goes.
    assert result == [
        [["resourcetimingbufferfull", True, False, True], ["resourcetimingbufferfull", True, False, True]],
        ["/one", "/two"],
        0,
    ]


@pytest.mark.usefixtures("page_program")
def test_a_response_from_the_cache_is_delivered_from_it(origins: tuple[_Origin, _Origin]) -> None:
    page, _ = origins
    page.routes["/"] = _html("<p>cache")
    page.routes["/cached"] = lambda: (200, [("Content-Type", "text/plain"), ("Cache-Control", "max-age=600")], b"kept")
    with mitto.Session(cache="memory") as session, _open_page(session, page.url("/")) as opened:
        result = opened.evaluate(
            """(async () => {
              await fetch('/cached').then((r) => r.text());
              await fetch('/cached').then((r) => r.text());
              return performance.getEntriesByType('resource').map((e) =>
                [e.deliveryType, e.transferSize, e.encodedBodySize, e.decodedBodySize]);
            })()"""
        )

    assert result == [["", 304, 4, 4], ["cache", 0, 4, 4]]
