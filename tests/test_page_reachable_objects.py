"""Tests of the objects a page reaches from its global object, and of what their attributes give (#215).

What each expression gives is what Chrome 157 gave for it: in the Script Fingerprint's Baseline,
tests/baselines/157/script/page.json, for a top-level page at an HTTPS URL, and, for what the probe does not read, such as
identities and iteration, in Mac Chrome 157.0.8103.51 at http://127.0.0.1, on 2026-11-13. Most run in the Page program,
where MITTO_PAGE_PROGRAM names it, on about:blank or on markup given with --html=; those of a loaded document run in a
Page that a Session opens on a local server's page, as a caller's does. docs/runtime.md, "The window", says what a page
reads.
"""

import http.server
import json
import os
import re
import subprocess
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.local_server import QuietServer
from tools.capture.probe import RecordValue, probe_script, read_record

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_REPOSITORY = Path(__file__).resolve().parents[1]

# The case page's markup, as the capture kit's Recording Case of a top-level page has it, without its script.
_CASE_MARKUP = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<p>Recording...</p>
<script></script>
</html>
"""


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str, html: str | None = None) -> Any:
    """What JSON.stringify() writes of what the script gives in the Page program, read back."""
    options = [] if html is None else [f"--html={html}"]
    result = subprocess.run(
        [program, *options, "-"],
        input=f"JSON.stringify({script})",
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )
    assert (result.returncode, result.stderr) == (0, "")
    return json.loads(result.stdout)


def test_a_top_level_window_is_its_own_top_parent_and_frames_and_has_no_opener(program: Path) -> None:
    script = "[frames === window, top === window, parent === window, opener, length, closed, name]"

    assert _evaluate(program, script) == [True, True, True, None, 0, False, ""]


def test_setting_the_opener_shadows_it_unless_it_is_null(program: Path) -> None:
    script = """(() => {
        opener = null;
        const kept = Object.getOwnPropertyDescriptor(window, "opener").get !== undefined;
        opener = 5;
        const shadowed = Object.getOwnPropertyDescriptor(window, "opener");
        return [kept, shadowed.value, shadowed.writable, shadowed.enumerable, shadowed.configurable];
    })()"""

    assert _evaluate(program, script) == [True, 5, True, True, True]


def test_the_window_of_about_blank_has_an_opaque_origin_in_a_secure_context(program: Path) -> None:
    script = "[origin, isSecureContext, originAgentCluster, offscreenBuffering, crossOriginIsolated]"

    assert _evaluate(program, script) == ["null", True, True, True, False]


def test_each_object_that_the_window_gives_is_the_same_one_each_time(program: Path) -> None:
    names = [
        "location",
        "history",
        "locationbar",
        "menubar",
        "personalbar",
        "scrollbars",
        "statusbar",
        "toolbar",
        "visualViewport",
        "viewport",
        "styleMedia",
        "navigation",
        "customElements",
        "crashReport",
        "trustedTypes",
        "documentPictureInPicture",
    ]
    script = f"[{', '.join(f'{name} === {name}' for name in names)}, locationbar === menubar]"

    assert _evaluate(program, script) == [True] * len(names) + [False]


def test_what_the_window_gives_holds_chromes_values(program: Path) -> None:
    script = """[
        Object.prototype.toString.call(crashReport),
        locationbar.visible, menubar.visible, personalbar.visible, scrollbars.visible, statusbar.visible,
        toolbar.visible,
        history.length, history.state, history.scrollRestoration,
        styleMedia.type,
        visualViewport.offsetLeft, visualViewport.offsetTop, visualViewport.pageLeft, visualViewport.pageTop,
        visualViewport.scale,
        trustedTypes.defaultPolicy, documentPictureInPicture.window,
        document.customElementRegistry === customElements, document.location === location,
    ]"""

    assert _evaluate(program, script) == [
        "[object CrashReportContext]",
        True,
        True,
        True,
        True,
        True,
        True,
        1,
        None,
        "auto",
        "screen",
        0,
        0,
        0,
        0,
        1,
        None,
        None,
        True,
        True,
    ]


def test_the_location_of_about_blank_gives_its_parts(program: Path) -> None:
    script = """[location.href, location.origin, location.protocol, location.host, location.hostname,
        location.port, location.pathname, location.search, location.hash, String(location),
        location.ancestorOrigins.length]"""

    assert _evaluate(program, script) == [
        "about:blank",
        "null",
        "about:",
        "",
        "",
        "",
        "blank",
        "",
        "",
        "about:blank",
        0,
    ]


def test_the_viewport_gives_a_new_array_of_one_new_segment_on_each_read(program: Path) -> None:
    script = """[viewport.segments.length, Object.isFrozen(viewport.segments), viewport.segments === viewport.segments,
        viewport.segments[0] === viewport.segments[0], Object.prototype.toString.call(viewport.segments[0]),
        viewport.segments[0].x, viewport.segments[0].y]"""

    assert _evaluate(program, script) == [1, True, False, False, "[object DOMRect]", 0, 0]


def test_a_document_of_an_opaque_origin_has_no_navigation_entries(program: Path) -> None:
    script = "[navigation.currentEntry, navigation.activation, navigation.entries(), navigation.transition]"

    assert _evaluate(program, script) == [None, None, [], None]


def test_the_documents_collections_are_live_and_the_same_each_time(program: Path) -> None:
    html = "<!doctype html><title>x</title><p>a</p><img><img id=i><a href=x name=n>a</a><a name=m></a><form></form>"
    script = """(() => {
        const names = ["images", "embeds", "plugins", "links", "forms", "scripts", "anchors", "applets", "all",
            "children", "childNodes"];
        const same = names.every((name) => document[name] === document[name]);
        const before = document.images.length;
        document.body.append(document.createElement("img"));
        return [same, document.plugins === document.embeds, before, document.images.length, document.links.length,
            document.anchors.length, document.forms.length, document.applets.length, document.images.i.nodeName,
            document.images.namedItem("i") === document.images[1], Object.keys(document.images),
            Object.getOwnPropertyNames(document.images), [...document.childNodes].map((node) => node.nodeName)];
    })()"""

    assert _evaluate(program, script, html) == [
        True,
        True,
        2,
        3,
        1,
        2,
        1,
        0,
        "IMG",
        True,
        ["0", "1", "2"],
        ["0", "1", "2", "i"],
        ["html", "HTML"],
    ]


def test_document_all_is_undetectable_and_holds_every_element(program: Path) -> None:
    html = "<!doctype html><title>x</title><p>a</p>"
    script = """[typeof document.all, document.all == null, document.all === undefined, document.all.length,
        [...Array(document.all.length).keys()].map((index) => document.all[index].nodeName)]"""

    assert _evaluate(program, script, html) == [
        "undefined",
        True,
        False,
        5,
        ["HTML", "HEAD", "TITLE", "BODY", "P"],
    ]


def test_a_nodes_children_and_child_nodes_follow_its_tree(program: Path) -> None:
    script = """(() => {
        const body = document.body;
        const nodes = body.childNodes;
        const children = body.children;
        body.append("text", document.createElement("p"));
        return [nodes === body.childNodes, children === body.children, nodes.length, children.length,
            nodes[0].nodeName, nodes.item(1).nodeName, children[0].nodeName, nodes.item(5),
            [...nodes].length, Object.prototype.toString.call(nodes)];
    })()"""

    assert _evaluate(program, script) == [True, True, 2, 1, "#text", "P", "P", None, 2, "[object NodeList]"]


@pytest.mark.parametrize(
    ("doctype", "scrolling"),
    [pytest.param("<!doctype html>", "HTML", id="standards"), pytest.param("", "BODY", id="quirks")],
)
def test_the_scrolling_element_follows_the_documents_mode(program: Path, doctype: str, scrolling: str) -> None:
    assert _evaluate(program, "document.scrollingElement.nodeName", f"{doctype}<p>a") == scrolling


def test_the_window_s_document_reads_as_a_tab_in_front_does(program: Path) -> None:
    script = """[document.domain, document.designMode, (document.designMode = "ON", document.designMode),
        document.activeElement.nodeName, document.visibilityState, document.webkitVisibilityState, document.hidden,
        document.webkitHidden, document.fullscreenEnabled, document.webkitFullscreenEnabled,
        document.pictureInPictureEnabled, document.fullscreenElement, document.webkitFullscreenElement,
        document.webkitCurrentFullScreenElement, document.rootElement, document.baseURI, document.fonts.status]"""

    assert _evaluate(program, script) == [
        "",
        "off",
        "on",
        "BODY",
        "visible",
        "visible",
        False,
        False,
        True,
        True,
        True,
        None,
        None,
        None,
        None,
        "about:blank",
        "loaded",
    ]


def test_a_document_of_no_window_has_no_location_and_is_hidden(program: Path) -> None:
    script = """(() => {
        const other = new Document();
        return [other.location, other.customElementRegistry, other.visibilityState, other.hidden,
            other.fullscreenEnabled, other.pictureInPictureEnabled];
    })()"""

    assert _evaluate(program, script) == [None, None, "hidden", True, False, False]


def test_a_rect_holds_its_coordinates_and_works_out_its_edges(program: Path) -> None:
    script = """(() => {
        const rect = new DOMRect(10, 20, -5, 8);
        const copy = DOMRectReadOnly.fromRect({x: 1, width: 2});
        const empty = new DOMRect(NaN);
        rect.width = 30;
        return [rect.x, rect.y, rect.width, rect.height, rect.top, rect.right, rect.bottom, rect.left,
            JSON.stringify(copy), Number.isNaN(empty.left), Object.prototype.toString.call(DOMRect.fromRect()),
            rect instanceof DOMRectReadOnly];
    })()"""

    assert _evaluate(program, script) == [
        10,
        20,
        30,
        8,
        20,
        40,
        28,
        10,
        '{"x":1,"y":0,"width":2,"height":0,"top":0,"right":3,"bottom":0,"left":1}',
        True,
        "[object DOMRect]",
        True,
    ]


def test_the_event_counts_count_none_of_each_type_in_chromes_order(program: Path) -> None:
    script = """(() => {
        const counts = performance.eventCounts;
        const each = [];
        counts.forEach((value, key, map) => each.push([key, value, map === counts]));
        return [counts === performance.eventCounts, counts.size, [...counts.keys()].slice(0, 4), each[0],
            counts.get("click"), counts.get("nothing"), counts.has("pointerleave"), counts.has("nothing"),
            [...counts.entries()][35], [...counts].length,
            Object.prototype.toString.call(counts.entries()), counts.entries().next.call(counts.keys()).value];
    })()"""

    assert _evaluate(program, script) == [
        True,
        36,
        ["pointerdown", "touchend", "input", "keydown"],
        ["pointerdown", 0, True],
        0,
        None,
        True,
        False,
        ["pointerleave", 0],
        36,
        "[object EventCounts Iterator]",
        "pointerdown",
    ]


def test_the_wgsl_language_features_are_chromes_in_its_order(program: Path) -> None:
    script = """[navigator.gpu.wgslLanguageFeatures.size, [...navigator.gpu.wgslLanguageFeatures][0],
        navigator.gpu.wgslLanguageFeatures.has("buffer_view"), [...navigator.gpu.wgslLanguageFeatures.entries()][12],
        navigator.gpu.wgslLanguageFeatures === navigator.gpu.wgslLanguageFeatures]"""

    assert _evaluate(program, script) == [
        13,
        "packed_4x8_integer_dot_product",
        True,
        ["uniform_buffer_standard_layout", "uniform_buffer_standard_layout"],
        True,
    ]


def test_static_attributes_give_chromes_values_and_the_same_arrays(program: Path) -> None:
    script = """[MediaSource.canConstructInDedicatedWorker, PushManager.supportedContentEncodings,
        PushManager.supportedContentEncodings === PushManager.supportedContentEncodings,
        Object.isFrozen(PushManager.supportedContentEncodings), PressureObserver.knownSources,
        PressureObserver.knownSources === PressureObserver.knownSources]"""

    assert _evaluate(program, script) == [True, ["aes128gcm", "aesgcm"], True, True, ["cpu"], True]


def test_a_known_gap_makes_objects_of_the_classes_that_allow_it(program: Path) -> None:
    # A Known Gap that gives an object of a class, or of an interface without a class below one, gets the object that
    # the class makes with nothing given (docs/runtime.md, "What a Known Gap does").
    script = """(() => {
        const found = document.getElementsByTagName("p");
        const source = new MediaSource();
        let heard = 0;
        source.addEventListener("x", () => heard++);
        source.dispatchEvent(new Event("x"));
        const registry = new CustomElementRegistry();
        return [Object.prototype.toString.call(found), found.length, found.item(0),
            Object.prototype.toString.call(document.querySelectorAll("p")),
            Object.prototype.toString.call(document.createElement("p").getBoundingClientRect()),
            heard, Object.prototype.toString.call(registry), registry === customElements];
    })()"""

    assert _evaluate(program, script) == [
        "[object HTMLCollection]",
        0,
        None,
        "[object NodeList]",
        "[object DOMRect]",
        1,
        "[object CustomElementRegistry]",
        False,
    ]


# A page whose script in the head reads what a page reads before its body is parsed.
_HEAD_SCRIPT_MARKUP = """<!doctype html><html><head><script>
window.inHead = [String(document.activeElement), document.body];
</script></head><body></body></html>"""

_LAST_MODIFIED = "Wed, 01 Jul 2026 08:09:10 GMT"


class _CasePages(http.server.BaseHTTPRequestHandler):
    """Answers /head with a page whose script runs in its head, /modified with the case page's markup and a
    Last-Modified, and any other path with the case page's markup."""

    def do_GET(self) -> None:
        body = (_HEAD_SCRIPT_MARKUP if self.path == "/head" else _CASE_MARKUP).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if self.path == "/modified":
            self.send_header("Last-Modified", _LAST_MODIFIED)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def page_url() -> Iterator[str]:
    """The URL of the case page on a local server."""
    server = QuietServer(("127.0.0.1", 0), _CasePages)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/script/page?x#y"
    finally:
        server.shutdown()
        server.server_close()


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session whose library has the Page program beside it, or skip."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def test_a_loaded_documents_window_location_and_navigation_give_its_url(session: mitto.Session, page_url: str) -> None:
    origin = page_url.split("/script")[0]
    host = origin.removeprefix("http://")
    script = """JSON.stringify([origin, location.href, location.origin, location.host, location.hostname, location.port,
        location.pathname, location.search, location.hash, document.domain, document.baseURI, navigation.entries().length,
        navigation.entries()[0] === navigation.currentEntry, navigation.activation.entry === navigation.currentEntry,
        navigation.activation.from, navigation.activation.navigationType, navigation.currentEntry.url,
        navigation.currentEntry.index, navigation.currentEntry.sameDocument, navigation.currentEntry.key.length,
        navigation.currentEntry.key === navigation.currentEntry.id, navigation.canGoBack, navigation.canGoForward,
        history.length])"""

    with _open_page(session, page_url) as page:
        values = json.loads(page.evaluate(script))

    assert values == [
        origin,
        page_url,
        origin,
        host,
        "127.0.0.1",
        host.split(":")[1],
        "/script/page",
        "?x",
        "#y",
        "127.0.0.1",
        page_url,
        1,
        True,
        True,
        None,
        "replace",
        page_url,
        0,
        True,
        36,
        False,
        False,
        False,
        1,
    ]


def test_with_no_body_nothing_is_the_active_element(session: mitto.Session, page_url: str) -> None:
    # Blink's TreeScope::activeElement() gives the body when nothing has focus, and so null before the parser has made
    # one, as Chrome 157 gave in a script in the head.
    with _open_page(session, page_url.split("/script")[0] + "/head") as page:
        assert json.loads(page.evaluate("JSON.stringify(window.inHead)")) == ["null", None]


def test_last_modified_is_the_responses_or_now_in_the_pages_time_zone(session: mitto.Session, page_url: str) -> None:
    # Blink writes lastModified as MM/DD/YYYY hh:mm:ss in local time, which for a Page is its time zone's, as Date's.
    origin = page_url.split("/script")[0]
    local = """(() => {
        const date = new Date("2026-07-01T08:09:10Z");
        const two = (value) => String(value).padStart(2, "0");
        return `${two(date.getMonth() + 1)}/${two(date.getDate())}/${date.getFullYear()} ` +
            `${two(date.getHours())}:${two(date.getMinutes())}:${two(date.getSeconds())}`;
    })()"""

    with _open_page(session, origin + "/modified") as page:
        modified, expected = json.loads(page.evaluate(f"JSON.stringify([document.lastModified, {local}])"))
    with _open_page(session, page_url) as page:
        now = json.loads(page.evaluate("JSON.stringify(document.lastModified)"))

    assert modified == expected
    assert re.fullmatch(r"\d{2}/\d{2}/\d{4} \d{2}:\d{2}:\d{2}", now)


def _paths(record: dict[str, RecordValue]) -> set[str]:
    objects = record["objects"]
    assert isinstance(objects, list)
    return {entry["path"] for entry in objects if isinstance(entry, dict) and isinstance(entry["path"], str)}


def test_the_probe_reaches_in_a_page_every_object_it_reaches_in_chrome(session: mitto.Session, page_url: str) -> None:
    # #215's measure, against the Baseline of the Engine Release. crypto.subtle waits for #222's Crypto, which keeps
    # SubtleCrypto refused. #213's Parity Checks compare everything else of each object.
    release = (_REPOSITORY / "tools/workspace/chromium_tag").read_text().split(".")[0]
    baseline = read_record((_REPOSITORY / f"tests/baselines/{release}/script/page.json").read_text(encoding="utf-8"))

    with _open_page(session, page_url) as page:
        record = read_record(page.evaluate(probe_script(), timeout=_TIMEOUT_SECONDS * 2))

    assert (_paths(baseline) - _paths(record), _paths(record) - _paths(baseline)) == ({"crypto.subtle"}, set())


def test_notification_and_performance_observer_give_their_static_values(program: Path) -> None:
    script = """(() => {
        let message = "";
        try {
            new PerformanceObserver();
        } catch (error) {
            message = error.message;
        }
        return [Notification.maxActions, PerformanceObserver.supportedEntryTypes,
            PerformanceObserver.supportedEntryTypes === PerformanceObserver.supportedEntryTypes,
            Object.isFrozen(PerformanceObserver.supportedEntryTypes), message,
            Object.prototype.toString.call(new PerformanceObserver(() => {})),
            new Notification("x") instanceof EventTarget];
    })()"""

    assert _evaluate(program, script) == [
        2,
        [
            "element",
            "event",
            "first-input",
            "interaction-contentful-paint",
            "largest-contentful-paint",
            "layout-shift",
            "long-animation-frame",
            "longtask",
            "mark",
            "measure",
            "navigation",
            "paint",
            "resource",
            "soft-navigation",
            "visibility-state",
        ],
        True,
        True,
        "Failed to construct 'PerformanceObserver': 1 argument required, but only 0 present.",
        "[object PerformanceObserver]",
        True,
    ]
