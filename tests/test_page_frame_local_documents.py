"""Tests of the documents that a Page's frame commits without the network, its srcdocs and about:blanks, on a local server.

The page is at /app/page, which sets a cookie for /app and, with ?rp=..., sends that Referrer-Policy; /echo and /app/echo
answer with the Referer and the Cookie of their request, or "none". Each test holds a frame's documents to what headless
Chrome for Testing 157.0.8103.79 gave for the same script on the same page, with the page's origin as ORIGIN: what they
take from their parent or from the document that started them, what they send, and how their navigations go.
"""

import http.server
import threading
import urllib.parse
from collections.abc import Iterator
from typing import Any

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
        result = page.evaluate(script)
        # The Page goes on: nothing that the script did made the library refuse it.
        assert page.evaluate("1 + 1") == 2
    return _anonymized(result, origin)


def _anonymized(value: Any, origin: str) -> Any:
    if isinstance(value, str):
        return value.replace(origin, "ORIGIN")
    if isinstance(value, list):
        return [_anonymized(item, origin) for item in value]
    if isinstance(value, dict):
        return {key: _anonymized(item, origin) for key, item in value.items()}
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
