"""Tests of the documents that a Page's frame commits without the network, its srcdocs and about:blanks, on a local server.

The page is at /app/page, which sets a cookie for /app and, with ?rp=..., sends that Referrer-Policy; /echo and /app/echo
answer with the Referer and the Cookie of their request, or "none", and /stall with a script two seconds later. Each path
under /assets/ answers with a module script and keeps its request's Referer, which /seen?path=... gives. Each test holds a frame's documents to what headless
Chrome for Testing 157.0.8103.79 gave for the same script on the same page, with the page's origin as ORIGIN: what they
take from their parent or from the document that started them, what they send, and how their navigations go.
"""

import http.server
import threading
import time
import urllib.parse
from collections.abc import Iterator
from typing import Any, cast

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_PAGE = b"<!doctype html><title>t</title><body><p id=x>top</p>"
_LOADED = (
    "const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));"
)
_SETTLE = "const settle = (ms) => new Promise((resolve) => setTimeout(resolve, ms));"
_ECHO = "const echo = (w, url, init) => w.eval(`fetch(${JSON.stringify(url)}, ${JSON.stringify(init || {})}).then(r => r.text())`);"


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def origin() -> Iterator[str]:
    """The origin of a server on 127.0.0.1 with the page at /app/page and the echoes."""
    seen: dict[str, str] = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            path = urllib.parse.urlsplit(self.path)
            headers: dict[str, str] = {}
            if path.path == "/app/page":
                body, content_type = _PAGE, "text/html"
                headers["Set-Cookie"] = "c1=v1; Path=/app"
                if policy := urllib.parse.parse_qs(path.query).get("rp"):
                    headers["Referrer-Policy"] = policy[0]
            elif path.path.startswith("/assets/"):
                seen[path.path] = self.headers.get("Referer") or "none"
                body, content_type = b"export default 1;", "text/javascript"
            elif path.path == "/seen":
                body = seen.get(urllib.parse.parse_qs(path.query)["path"][0], "unseen").encode()
                content_type = "text/plain"
            elif path.path == "/stall":
                # A script that keeps its parser waiting a while.
                time.sleep(2)
                body, content_type = b"parent.stalled = true;", "text/javascript"
            elif path.path.endswith("/echo"):
                body = f"{self.headers.get('Referer') or 'none'}|{self.headers.get('Cookie') or 'none'}".encode()
                content_type = "text/plain"
            else:
                body, content_type = b"<p>other", "text/html"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            for name, value in headers.items():
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()
    server.server_close()


def _evaluate(origin: str, script: str, query: str = "") -> Any:
    """What `script` gives in a Page at /app/page, with the origin as ORIGIN in each string."""
    url = f"{origin}/app/page" + (f"?{query}" if query else "")
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        result: object = page.evaluate(script, timeout=_TIMEOUT_SECONDS)
        # The Page goes on: nothing that the script did made the library refuse it.
        assert page.evaluate("1 + 1", timeout=_TIMEOUT_SECONDS) == 2
    return _anonymized(result, origin)


def _anonymized(value: object, origin: str) -> object:
    if isinstance(value, str):
        return value.replace(origin, "ORIGIN")
    if isinstance(value, list):
        return [_anonymized(item, origin) for item in cast(list[object], value)]
    if isinstance(value, dict):
        return {key: _anonymized(item, origin) for key, item in cast(dict[str, object], value).items()}
    return value


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_about_blank_and_srcdoc_take_their_parent_s_cookie_url(origin: str) -> None:
    # Both read the cookie of /app, which their parent at /app/page reads, and one that they set with no path gets /app.
    script = f"""
      (async () => {{
        {_LOADED}
        const blank = document.createElement('iframe');
        document.body.appendChild(blank);
        const fromBlank = blank.contentDocument.cookie;
        blank.contentDocument.cookie = 'c2=fromblank';
        const srcdoc = document.createElement('iframe');
        srcdoc.srcdoc = '<p>s';
        document.body.appendChild(srcdoc);
        await loaded(srcdoc);
        const fromSrcdoc = srcdoc.contentDocument.cookie;
        srcdoc.contentDocument.cookie = 'c3=fromsrcdoc';
        return [fromBlank, fromSrcdoc, await (await fetch('/app/echo')).text(), await (await fetch('/echo')).text()];
      }})()
    """
    assert _evaluate(origin, script) == [
        "c1=v1",
        "c1=v1; c2=fromblank",
        "ORIGIN/app/page|c1=v1; c2=fromblank; c3=fromsrcdoc",
        "ORIGIN/app/page|none",
    ]


@pytest.mark.usefixtures("page_program")
def test_an_about_blank_frame_sends_no_referer_under_any_policy_but_for_a_referrer_that_fetch_names(
    origin: str,
) -> None:
    script = f"""
      (async () => {{
        {_ECHO}
        const blank = document.createElement('iframe');
        document.body.appendChild(blank);
        const w = blank.contentWindow;
        return [
          await echo(w, '/echo'),
          await echo(w, '/echo', {{referrerPolicy: 'origin'}}),
          await echo(w, '/echo', {{referrerPolicy: 'unsafe-url'}}),
          await echo(w, '/echo', {{referrer: '/x'}}),
        ];
      }})()
    """
    assert _evaluate(origin, script) == ["none|none", "none|none", "none|none", "ORIGIN/x|none"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_documents_keep_their_parent_s_referrer_policy(origin: str) -> None:
    # Under Referrer-Policy: no-referrer, a srcdoc's requests send no Referer, and neither does the frame's initial
    # about:blank nor a later one with a referrer that fetch() names.
    script = f"""
      (async () => {{
        {_LOADED}
        {_ECHO}
        const srcdoc = document.createElement('iframe');
        srcdoc.srcdoc = '<p>s';
        document.body.appendChild(srcdoc);
        await loaded(srcdoc);
        const fromSrcdoc = await echo(srcdoc.contentWindow, '/echo');
        const initial = document.createElement('iframe');
        document.body.appendChild(initial);
        const fromInitial = await echo(initial.contentWindow, '/echo', {{referrer: '/x'}});
        srcdoc.removeAttribute('srcdoc');
        await loaded(srcdoc);
        return [fromSrcdoc, fromInitial, await echo(srcdoc.contentWindow, '/echo', {{referrer: '/x'}})];
      }})()
    """
    assert _evaluate(origin, script, "rp=no-referrer") == ["none|none", "none|none", "none|none"]


@pytest.mark.parametrize(
    ("query", "referrer"),
    [("", "ORIGIN/"), ("rp=unsafe-url", "ORIGIN/app/page?rp=unsafe-url"), ("rp=no-referrer", "")],
)
@pytest.mark.usefixtures("page_program")
def test_a_frame_s_srcdoc_and_about_blank_have_the_referrer_that_their_navigation_gives(
    origin: str, query: str, referrer: str
) -> None:
    # The parent's URL under its referrer policy for a navigation to about:srcdoc or about:blank, of another origin.
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        const fromSrcdoc = element.contentDocument.referrer;
        element.removeAttribute('srcdoc');
        await loaded(element);
        const fromRemoval = [element.contentDocument.URL, element.contentDocument.referrer];
        element.srcdoc = '<p>b';
        await loaded(element);
        element.src = 'about:blank';
        element.removeAttribute('srcdoc');
        await loaded(element);
        return [fromSrcdoc, fromRemoval, [element.contentDocument.URL, element.contentDocument.referrer]];
      }})()
    """
    assert _evaluate(origin, script, query) == [referrer, ["about:blank", referrer], ["about:blank", referrer]]


@pytest.mark.usefixtures("page_program")
def test_a_parent_that_sets_its_frame_s_location_starts_its_about_blank(origin: str) -> None:
    # The about:blank is the parent's, with the parent's base URL, not the srcdoc's own base element's.
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<base href="/base/"><p>l';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.location.href = 'about:blank';
        await loaded(element);
        return [element.contentDocument.URL, element.contentDocument.referrer, element.contentDocument.baseURI];
      }})()
    """
    assert _evaluate(origin, script) == ["about:blank", "ORIGIN/", "ORIGIN/app/page"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_stays_in_no_quirks_mode_and_times_its_load(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>s';
        document.body.appendChild(element);
        await loaded(element);
        const first = element.contentDocument.compatMode;
        element.srcdoc = '<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 3.2//EN"><p>t';
        await loaded(element);
        return [first, element.contentDocument.compatMode, element.contentWindow.performance.timing.domComplete > 0];
      }})()
    """
    assert _evaluate(origin, script) == ["CSS1Compat", "CSS1Compat", True]


@pytest.mark.usefixtures("page_program")
def test_document_open_cancels_a_frame_s_srcdoc_that_waits(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        element.srcdoc = '<p>b';
        element.contentDocument.open();
        element.contentDocument.write('<p>x');
        element.contentDocument.close();
        await settle(500);
        return element.contentDocument.body.textContent;
      }})()
    """
    assert _evaluate(origin, script) == "x"


@pytest.mark.usefixtures("page_program")
def test_a_newer_navigation_cancels_a_frame_s_srcdoc_that_waits(origin: str) -> None:
    # Taking srcdoc away navigates to the src, which Chrome loads: the srcdoc that waited never runs. A frame's
    # navigation through the Session is still a Known Gap, so the frame stays at its first srcdoc.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.src = '/other';
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        element.srcdoc = '<script>parent.ran = true<\\/script>';
        element.removeAttribute('srcdoc');
        await settle(1000);
        return window.ran === true;
      }})()
    """
    assert _evaluate(origin, script) is False


@pytest.mark.usefixtures("page_program")
def test_a_sandboxed_frame_s_srcdoc_runs_no_script(origin: str) -> None:
    script = f"""
      (async () => {{
        {_SETTLE}
        const element = document.createElement('iframe');
        element.sandbox = '';
        element.srcdoc = '<script>parent.ran = true<\\/script>';
        document.body.appendChild(element);
        await settle(500);
        return window.ran === true;
      }})()
    """
    assert _evaluate(origin, script) is False


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_its_pagehide_listener_removes_commits_no_srcdoc(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.addEventListener('pagehide', () => element.remove());
        element.srcdoc = '<p>b';
        await settle(500);
        return [element.isConnected, element.contentWindow === null];
      }})()
    """
    assert _evaluate(origin, script) == [False, True]


@pytest.mark.usefixtures("page_program")
def test_an_about_blank_with_a_query_navigates_within_itself(origin: str) -> None:
    # Whether it committed at once, as a frame's first about:blank does, or from a later task.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const first = document.createElement('iframe');
        first.src = 'about:blank?v=1';
        document.body.appendChild(first);
        first.contentWindow.location.hash = 'h';
        const later = document.createElement('iframe');
        later.srcdoc = '<p>q';
        document.body.appendChild(later);
        await loaded(later);
        later.removeAttribute('srcdoc');
        later.src = 'about:blank?v=2';
        await loaded(later);
        later.contentWindow.location.hash = 'h';
        await settle(100);
        return [first.contentDocument.URL, later.contentDocument.URL];
      }})()
    """
    assert _evaluate(origin, script) == ["about:blank?v=1#h", "about:blank?v=2#h"]


@pytest.mark.usefixtures("page_program")
def test_each_frame_counts_its_own_navigations(origin: str) -> None:
    # 101 srcdocs in a row in each of two frames: 202 navigations in all, past 200, but each frame's are fewer.
    script = f"""
      (async () => {{
        {_SETTLE}
        const elements = [document.createElement('iframe'), document.createElement('iframe')];
        for (const element of elements) {{
          document.body.appendChild(element);
        }}
        for (const element of elements) {{
          for (let i = 1; i <= 101; i++) {{
            element.srcdoc = `<p>${{i}}`;
          }}
        }}
        await settle(1000);
        return elements.map((element) => element.contentDocument.body && element.contentDocument.body.textContent);
      }})()
    """
    assert _evaluate(origin, script) == ["101", "101"]


@pytest.mark.usefixtures("page_program")
def test_a_sandboxed_frame_runs_no_javascript_url(origin: str) -> None:
    script = f"""
      (async () => {{
        {_SETTLE}
        const fromSrc = document.createElement('iframe');
        fromSrc.sandbox = '';
        fromSrc.src = 'javascript:parent.ranSrc = true; "x"';
        document.body.appendChild(fromSrc);
        await settle(300);
        const fromLocation = document.createElement('iframe');
        fromLocation.sandbox = '';
        document.body.appendChild(fromLocation);
        await settle(100);
        try {{
          fromLocation.contentWindow.location = 'javascript:parent.ranLocation = true; "y"';
        }} catch (e) {{}}
        await settle(300);
        return [window.ranSrc === true, window.ranLocation === true];
      }})()
    """
    assert _evaluate(origin, script) == [False, False]


@pytest.mark.usefixtures("page_program")
def test_an_about_blank_whose_initiator_goes_first_still_takes_what_it_had(origin: str) -> None:
    # A frame navigates its sibling to about:blank and is removed before that commits: the about:blank still takes the
    # initiator's origin and cookie URL, as Chrome's browser took them when the navigation started.
    script = f"""
      (async () => {{
        {_LOADED}
        const target = document.createElement('iframe');
        target.srcdoc = '<p>t';
        document.body.appendChild(target);
        await loaded(target);
        const starter = document.createElement('iframe');
        document.body.appendChild(starter);
        starter.contentWindow.eval("parent.frames[0].location.href = 'about:blank'");
        starter.remove();
        await loaded(target);
        const d = target.contentDocument;
        return [d.URL, d.referrer, d.cookie];
      }})()
    """
    assert _evaluate(origin, script) == ["about:blank", "", "c1=v1"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_at_a_url_with_a_query_navigates_within_itself(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>orig';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.location = 'about:srcdoc?x';
        await loaded(element);
        element.contentWindow.location.hash = 'h';
        await settle(100);
        return [element.contentDocument.URL, element.contentDocument.body.textContent];
      }})()
    """
    assert _evaluate(origin, script) == ["about:srcdoc?x#h", "orig"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_old_window_keeps_its_own_document(origin: str) -> None:
    # The first srcdoc's Request, kept after the second srcdoc took the frame's WindowProxy, resolves against the first
    # document's base URL.
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<base href="/one/"><p>1';
        document.body.appendChild(element);
        await loaded(element);
        const OldRequest = element.contentWindow.Request;
        element.srcdoc = '<base href="/two/"><p>2';
        await loaded(element);
        return [new OldRequest('x').url, new element.contentWindow.Request('x').url];
      }})()
    """
    assert _evaluate(origin, script) == ["ORIGIN/one/x", "ORIGIN/two/x"]


@pytest.mark.usefixtures("page_program")
def test_a_data_url_navigation_cancels_a_frame_s_srcdoc_that_waits(origin: str) -> None:
    # Chrome loads the data: URL; a frame's navigation to one is still a Known Gap, but the srcdoc that waited never runs.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.src = 'data:text/html,<p>d';
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        element.srcdoc = '<script>parent.ran = true<\\/script>';
        element.removeAttribute('srcdoc');
        await settle(1000);
        return window.ran === true;
      }})()
    """
    assert _evaluate(origin, script) is False


@pytest.mark.usefixtures("page_program")
def test_an_iframe_s_referrerpolicy_decides_its_srcdoc_s_referrer(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        const referrers = [];
        for (const policy of ['no-referrer', 'unsafe-url']) {{
          const element = document.createElement('iframe');
          element.referrerPolicy = policy;
          element.srcdoc = '<p>p';
          document.body.appendChild(element);
          await loaded(element);
          referrers.push(element.contentDocument.referrer);
        }}
        return referrers;
      }})()
    """
    assert _evaluate(origin, script) == ["", "ORIGIN/app/page"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_document_that_its_parent_opens_sends_the_parent_s_url_as_referer(origin: str) -> None:
    script = f"""
      (async () => {{
        {_ECHO}
        const element = document.createElement('iframe');
        document.body.appendChild(element);
        element.contentDocument.open();
        element.contentDocument.write('<p>w');
        element.contentDocument.close();
        return [element.contentDocument.URL, await echo(element.contentWindow, '/echo')];
      }})()
    """
    assert _evaluate(origin, script) == ["ORIGIN/app/page", "ORIGIN/app/page|none"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_its_beforeunload_listener_removes_starts_no_navigation(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>b';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.addEventListener('beforeunload', () => element.remove());
        element.removeAttribute('srcdoc');
        await settle(300);
        return [element.isConnected, element.contentWindow === null];
      }})()
    """
    assert _evaluate(origin, script) == [False, True]


@pytest.mark.usefixtures("page_program")
def test_a_replaced_srcdoc_document_has_no_cookies(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>c1';
        document.body.appendChild(element);
        await loaded(element);
        const old = element.contentDocument;
        element.srcdoc = '<p>c2';
        await loaded(element);
        const read = old.cookie;
        old.cookie = 'old=1';
        return [read, document.cookie];
      }})()
    """
    assert _evaluate(origin, script) == ["", "c1=v1"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_document_that_an_about_blank_opens_takes_its_url(origin: str) -> None:
    # The about:blank frame's own task opens its sibling's document, which takes about:blank and so sends no Referer.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        {_ECHO}
        const target = document.createElement('iframe');
        target.srcdoc = '<p>t';
        document.body.appendChild(target);
        await loaded(target);
        const opener = document.createElement('iframe');
        document.body.appendChild(opener);
        opener.contentWindow.eval(
          "setTimeout(() => {{ const d = parent.frames[0].document; d.open(); d.write('<p>o'); d.close(); }}, 0)");
        await settle(300);
        const d = target.contentDocument;
        return [d.URL, d.body.textContent, await echo(target.contentWindow, '/echo')];
      }})()
    """
    assert _evaluate(origin, script) == ["about:blank", "o", "none|none"]


@pytest.mark.usefixtures("page_program")
def test_a_fragment_navigation_leaves_a_frame_s_srcdoc_that_waits(origin: str) -> None:
    # Chrome commits the waiting srcdoc after the navigation within the document.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a';
        document.body.appendChild(element);
        await loaded(element);
        element.srcdoc = '<p>queued';
        element.contentWindow.location.hash = 'x';
        await settle(500);
        return [element.contentDocument.URL, element.contentDocument.body.textContent];
      }})()
    """
    assert _evaluate(origin, script) == ["about:srcdoc", "queued"]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_srcdoc_and_about_blank_have_navigation_entries(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const entry = (w) => w.performance.getEntriesByType('navigation').map((e) => [
          e.name, e.type, e.deliveryType, e.fetchStart, e.transferSize, e.responseEnd > 0, e.domComplete > 0,
          e.encodedBodySize > 0,
        ]);
        const element = document.createElement('iframe');
        element.srcdoc = '<p>s';
        document.body.appendChild(element);
        await loaded(element);
        await settle(50);
        const fromSrcdoc = entry(element.contentWindow);
        element.removeAttribute('srcdoc');
        await loaded(element);
        await settle(50);
        const fromBlank = entry(element.contentWindow);
        const synchronous = document.createElement('iframe');
        document.body.appendChild(synchronous);
        await settle(50);
        return [fromSrcdoc, fromBlank, entry(synchronous.contentWindow)];
      }})()
    """
    assert _evaluate(origin, script) == [
        [["about:srcdoc", "navigate", "cache", 0, 0, True, True, True]],
        [["about:blank", "navigate", "cache", 0, 0, True, True, False]],
        [["about:blank", "navigate", "cache", 0, 0, True, True, False]],
    ]


@pytest.mark.usefixtures("page_program")
def test_a_frame_that_its_readystatechange_listener_removes_as_it_aborts_starts_no_navigation(origin: str) -> None:
    # Taking srcdoc away while the srcdoc's parser waits for a script aborts its document, whose readystatechange
    # listener removes the frame.
    script = f"""
      (async () => {{
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<script>document.onreadystatechange = () => {{ if (document.readyState === "complete") {{ '
          + 'parent.ranReady = true; frameElement.remove(); }} }};<\\/script><script src="/stall"><\\/script>';
        document.body.appendChild(element);
        for (let i = 0; i < 100; i++) {{
          const d = element.contentDocument;
          if (d && d.URL === 'about:srcdoc' && d.readyState === 'loading') break;
          await settle(10);
        }}
        element.removeAttribute('srcdoc');
        await settle(300);
        return [window.ranReady === true, element.isConnected];
      }})()
    """
    assert _evaluate(origin, script) == [True, False]


@pytest.mark.usefixtures("page_program")
def test_a_replaced_srcdoc_s_cookie_store_asks_nothing(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const outcome = (p) => Promise.race([
          p.then(() => 'resolved', (e) => e.name), settle(1000).then(() => 'pending'),
        ]);
        const element = document.createElement('iframe');
        element.srcdoc = '<p>c1';
        document.body.appendChild(element);
        await loaded(element);
        const old = element.contentWindow.cookieStore;
        element.srcdoc = '<p>c2';
        await loaded(element);
        return [await outcome(old.get('c1')), await outcome(old.set('old', '1')), document.cookie];
      }})()
    """
    assert _evaluate(origin, script) == ["pending", "pending", "c1=v1"]


@pytest.mark.usefixtures("page_program")
def test_a_cookie_listener_of_the_initial_window_hears_after_its_srcdoc_commits(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>l';
        document.body.appendChild(element);
        const changes = [];
        element.contentWindow.cookieStore.addEventListener(
          'change', (e) => changes.push(e.changed.map((c) => c.name).join(',')));
        await loaded(element);
        await cookieStore.set({{name: 'seen', value: '1', path: '/app'}});
        await settle(500);
        return changes;
      }})()
    """
    assert _evaluate(origin, script) == ["seen"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_that_reloads_counts_as_a_reload(origin: str) -> None:
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>r';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.location.reload();
        await loaded(element);
        const w = element.contentWindow;
        return [
          w.performance.getEntriesByType('navigation').map((e) => e.type), w.performance.navigation.type,
          element.contentDocument.body.textContent,
        ];
      }})()
    """
    assert _evaluate(origin, script) == [["reload"], 1, "r"]


@pytest.mark.usefixtures("page_program")
def test_a_top_level_document_that_a_frame_at_a_query_opens_takes_its_url(origin: str) -> None:
    # The frame committed at about:blank?v=1 at once; its own task opens the top-level document, which takes that URL,
    # and so does the Page.
    script = """
      (async () => {
        const element = document.createElement('iframe');
        element.src = 'about:blank?v=1';
        document.body.appendChild(element);
        element.contentWindow.eval(
          "setTimeout(() => { top.document.open(); top.document.write('<p>o'); top.document.close(); }, 0)");
        await new Promise((resolve) => setTimeout(resolve, 300));
        return document.URL;
      })()
    """
    with (
        mitto.Session() as session,
        session._open_page(f"{origin}/app/page", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate(script, timeout=_TIMEOUT_SECONDS) == "about:blank?v=1"
        assert page.url == "about:blank?v=1"


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_child_that_navigates_it_as_it_commits_starts_nothing(origin: str) -> None:
    # The child's pagehide, as the frame's next srcdoc commits, sets the frame's location: nothing starts.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>a<iframe></iframe>';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.frames[0].addEventListener(
          'pagehide', () => {{ element.contentWindow.location.href = 'about:blank'; }});
        element.srcdoc = '<p>b';
        await settle(500);
        return [element.contentDocument.URL, element.contentDocument.body.textContent];
      }})()
    """
    assert _evaluate(origin, script) == ["about:srcdoc", "b"]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_frame_in_a_sandboxed_frame_runs_no_script(origin: str) -> None:
    script = f"""
      (async () => {{
        {_SETTLE}
        const sandboxed = document.createElement('iframe');
        sandboxed.sandbox = 'allow-same-origin';
        document.body.appendChild(sandboxed);
        await settle(100);
        const nested = sandboxed.contentDocument.createElement('iframe');
        nested.srcdoc = '<script>top.ran = true<\\/script>';
        sandboxed.contentDocument.body.appendChild(nested);
        await settle(500);
        return window.ran === true;
      }})()
    """
    assert _evaluate(origin, script) is False


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_load_waits_no_more_for_a_srcdoc_that_its_navigation_aborts(origin: str) -> None:
    # The inner srcdoc waits for a script, so the outer frame's load waits for it, until taking srcdoc away navigates
    # the inner frame to its src, which aborts the srcdoc. Chrome loads the src; a frame's navigation through the
    # Session is still a Known Gap, but the outer frame loads either way.
    script = f"""
      (async () => {{
        {_SETTLE}
        const outer = document.createElement('iframe');
        outer.srcdoc = '<iframe src="/other" srcdoc="<script src=/stall></script>"></iframe>';
        let outerLoaded = false;
        outer.addEventListener('load', () => {{ outerLoaded = true; }});
        document.body.appendChild(outer);
        for (let i = 0; i < 100; i++) {{
          const inner = outer.contentDocument && outer.contentDocument.querySelector('iframe');
          const d = inner && inner.contentDocument;
          if (d && d.URL === 'about:srcdoc' && d.readyState === 'loading') break;
          await settle(10);
        }}
        const before = outerLoaded;
        outer.contentDocument.querySelector('iframe').removeAttribute('srcdoc');
        await settle(1000);
        return [before, outerLoaded];
      }})()
    """
    assert _evaluate(origin, script) == [False, True]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_keeps_its_parent_s_cookie_url_after_the_parent_navigates_within_itself(origin: str) -> None:
    # The parent navigates within itself to /app/sub/x while its frame's srcdoc waits: both keep the cookie URL that
    # the parent was made at, so a cookie that either sets with no path is /app's. Each writes over a pipe of its own
    # in Chrome, so the cookies' order is sorted.
    script = f"""
      (async () => {{
        {_LOADED}
        const intercepted = (url) => {{
          navigation.addEventListener('navigate', (e) => {{ if (e.canIntercept) e.intercept(); }}, {{once: true}});
          return navigation.navigate(url).committed;
        }};
        const element = document.createElement('iframe');
        element.srcdoc = '<p>c';
        document.body.appendChild(element);
        await intercepted('/app/sub/x');
        await loaded(element);
        element.contentDocument.cookie = 'q=1';
        document.cookie = 'p=1';
        const fromSrcdoc = await element.contentWindow.cookieStore.get({{name: 'c1', url: '/app/page'}})
          .then((v) => v ? v.name : null, (e) => e.name);
        const sorted = async (path) => {{
          const [referrer, cookies] = (await (await fetch(path)).text()).split('|');
          return referrer + '|' + cookies.split('; ').sort().join('; ');
        }};
        const result = [location.pathname, fromSrcdoc, await sorted('/app/echo'), await sorted('/app/sub/echo')];
        await intercepted('/app/page');
        return result;
      }})()
    """
    assert _evaluate(origin, script) == [
        "/app/sub/x",
        "c1",
        "ORIGIN/app/sub/x|c1=v1; p=1; q=1",
        "ORIGIN/app/sub/x|c1=v1; p=1; q=1",
    ]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_s_navigation_counts_from_its_start(origin: str) -> None:
    # The old srcdoc's slow pagehide counts in the new one's navigation, whose time origin is the navigation's start;
    # a srcdoc shows no unload times.
    script = f"""
      (async () => {{
        {_LOADED}
        const element = document.createElement('iframe');
        element.srcdoc = '<p>t1';
        document.body.appendChild(element);
        await loaded(element);
        element.contentWindow.addEventListener('pagehide', () => {{
          const end = Date.now() + 300;
          while (Date.now() < end) {{}}
        }});
        element.srcdoc = '<p>t2';
        await loaded(element);
        const e = element.contentWindow.performance.getEntriesByType('navigation')[0];
        return [e.domInteractive > 250, e.unloadEventStart, e.unloadEventEnd, e.startTime];
      }})()
    """
    assert _evaluate(origin, script) == [True, 0, 0, 0]


@pytest.mark.usefixtures("page_program")
def test_a_frame_s_navigation_fires_beforeunload_at_its_frames_too(origin: str) -> None:
    # beforeunload at the frame's document, then at its frame's, before either unloads; a navigation that the inner
    # one's listener starts of the outer frame starts nothing. Whether unload fires is a Known Gap, which this leaves
    # out.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const order = [];
        const outer = document.createElement('iframe');
        outer.srcdoc = '<p>outer<iframe srcdoc="<p>inner"></iframe>';
        document.body.appendChild(outer);
        await loaded(outer);
        await settle(100);
        const inner = outer.contentWindow.frames[0];
        for (const [name, w] of [['outer', outer.contentWindow], ['inner', inner]]) {{
          for (const type of ['beforeunload', 'pagehide']) {{
            w.addEventListener(type, () => order.push(name + ' ' + type));
          }}
        }}
        inner.addEventListener('beforeunload', () => {{ outer.contentWindow.location.href = '/child?fromInner'; }});
        outer.removeAttribute('srcdoc');
        await loaded(outer);
        await settle(200);
        return [order, outer.contentDocument.URL];
      }})()
    """
    assert _evaluate(origin, script) == [
        ["outer beforeunload", "inner beforeunload", "outer pagehide", "inner pagehide"],
        "about:blank",
    ]


@pytest.mark.usefixtures("page_program")
def test_a_srcdoc_s_module_sends_its_base_url_as_what_it_imports_referer(origin: str) -> None:
    # The inline module's base URL, of the srcdoc's base element, is the referrer of what it imports, as it is of the
    # parent's origin, which the srcdoc took.
    script = f"""
      (async () => {{
        {_LOADED}
        {_SETTLE}
        const element = document.createElement('iframe');
        element.srcdoc = '<base href="/assets/"><script type=module>import "./dep.js"; parent.imported = true;<\\/script>';
        document.body.appendChild(element);
        await loaded(element);
        for (let i = 0; i < 100 && !window.imported; i++) await settle(10);
        return [window.imported === true, await (await fetch('/seen?path=/assets/dep.js')).text()];
      }})()
    """
    assert _evaluate(origin, script) == [True, "ORIGIN/assets/"]
