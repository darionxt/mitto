"""Tests of lazy frames and of credentials in a frame's URL, on a local server.

Each case is a page whose script logs its own events, its frames' load events and what their documents tell it, while the server keeps
which children it was asked for, with their Referer lines. The expected values are what headless Chrome for Testing 157.0.8103.79 gave
for the same pages, the same each time, twice or more, with the console for the credentials.
"""

from __future__ import annotations

import contextlib
import http.server
import threading
import time
from collections.abc import Generator
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_GRACE_SECONDS = 0.3
# How long the server holds the rest of a gated page's body at most.
_GATE_SECONDS = 10
_HEAD = """<!doctype html><script>
const said = [];
const log = (text) => said.push(text);
onmessage = ({data}) => log(data);
addEventListener('DOMContentLoaded', () => log('DOMContentLoaded'));
addEventListener('load', () => log('parent load'));
const where = (frame) => {
  try {
    return frame.contentWindow.location.href.replace(/^http:\\/\\/[^/]*/, '');
  } catch (error) {
    return 'cross';
  }
};
</script>"""
_CHILD = "<!doctype html><script>parent.postMessage('child ran ' + location.search, '*');</script>"
# Where the server holds the rest of a page's body until the page fetches /release, so that what the page waits for happens while it
# parses, as it does in Chrome, whose first rendering opportunity comes before DOMContentLoaded.
_GATE = "<!--gate-->"
_RELEASE = "; fetch('/release')"


def _frame(name: str, extra: str = "", src: str | None = None, then: str = "") -> str:
    source = src if src is not None else f"/child?{name}"
    return (
        f"<iframe id={name} {extra} src='{source}' onload=\"log('frame load {name} ' + where(this)){then}\"></iframe>"
    )


_LAZY: dict[str, tuple[str, list[str], list[str]]] = {
    "parsed": (
        _frame("a", "loading=lazy", then=_RELEASE)
        + "<script>log('script ' + where(document.querySelector('iframe')))</script>"
        + _GATE,
        ["script about:blank", "frame load a /child?a", "DOMContentLoaded", "parent load"],
        ["/child?a referer http://A/parsed"],
    ),
    "eager": (
        _frame("a", "loading=lazy")
        + "<script>document.querySelector('iframe').loading = 'eager'; log('set eager')</script>",
        ["set eager", "DOMContentLoaded", "frame load a /child?a", "parent load"],
        ["/child?a referer http://A/eager"],
    ),
    "removed": (
        _frame("a", "loading=lazy")
        + "<script>document.querySelector('iframe').removeAttribute('loading'); log('removed loading')</script>",
        ["removed loading", "DOMContentLoaded", "frame load a /child?a", "parent load"],
        ["/child?a referer http://A/removed"],
    ),
    "navigated": (
        _frame("a", "loading=lazy")
        + "<script>document.querySelector('iframe').contentWindow.location.href = '/child?b'; log('navigated')</script>",
        ["navigated", "DOMContentLoaded", "frame load a /child?b", "parent load"],
        ["/child?b referer http://A/navigated"],
    ),
    "src": (
        _frame("a", "loading=lazy", then=_RELEASE)
        + "<script>document.querySelector('iframe').src = '/child?b'; log('src b')</script>"
        + _GATE,
        ["src b", "frame load a /child?b", "DOMContentLoaded", "parent load"],
        ["/child?b referer http://A/src"],
    ),
    "nosrc": (
        _frame("a", "loading=lazy")
        + "<script>document.querySelector('iframe').removeAttribute('src');"
        + " log('no src ' + where(document.querySelector('iframe')))</script>",
        ["frame load a about:blank", "no src about:blank", "DOMContentLoaded", "parent load"],
        [],
    ),
    "policy": (
        _frame("a", "loading=lazy referrerpolicy=no-referrer", then=_RELEASE)
        + "<script>document.querySelector('iframe').referrerPolicy = 'unsafe-url'; log('unsafe-url')</script>"
        + _GATE,
        ["unsafe-url", "frame load a /child?a", "DOMContentLoaded", "parent load"],
        ["/child?a"],
    ),
    "inserted": (
        "<body><script>addEventListener('load', () => {"
        f" document.body.insertAdjacentHTML('beforeend', `{_frame('a', 'loading=lazy')}`); log('inserted');"
        " requestAnimationFrame(() => log('raf')); });</script>",
        ["DOMContentLoaded", "parent load", "inserted", "raf", "frame load a /child?a"],
        ["/child?a referer http://A/inserted"],
    ),
    "detached": (
        _frame("a", "loading=lazy") + "<script>const f = document.querySelector('iframe'); f.remove(); log('removed');"
        " setTimeout(() => { document.body.append(f); log('back'); }, 200);</script>",
        ["removed", "DOMContentLoaded", "parent load", "back", "frame load a /child?a"],
        ["/child?a referer http://A/detached"],
    ),
    "srcdoc": (
        "<iframe loading=lazy srcdoc='<script>parent.postMessage(`srcdoc ran`, `*`)</script>'"
        " onload=\"log('frame load srcdoc')\"></iframe>",
        ["DOMContentLoaded", "frame load srcdoc", "parent load", "srcdoc ran"],
        [],
    ),
    "blank": (
        "<iframe loading=lazy src='about:blank' onload=\"log('frame load blank')\"></iframe>",
        ["frame load blank", "DOMContentLoaded", "parent load"],
        [],
    ),
    "noscript": (
        "<iframe sandbox src='/inner' onload=\"log('frame load inner')\"></iframe>",
        ["DOMContentLoaded", "frame load inner", "parent load"],
        ["/inner referer http://A/noscript", "/child?innerchild"],
    ),
    "nested": (
        "<script>addEventListener('message', ({data}) => { if (data === 'deep load') fetch('/release'); });</script>"
        + _frame("outer", "loading=lazy")
        + _GATE,
        ["frame load outer /child?outer", "deep load", "DOMContentLoaded", "parent load"],
        ["/child?outer referer http://A/nested", "/child?deep referer http://A/child?outer"],
    ),
}

_WARNING = (
    "Subresource requests whose URLs contain embedded credentials (e.g. `https://user:pass@host/`) are blocked. See "
    "https://www.chromestatus.com/feature/5669008342777856 for more details."
)
# The cases of credentials, by name: whether the page's own URL has u:p, then what it logs, its frame f's URL at the end, what the
# server was asked for, and whether the console has Chrome's warning.
_CREDENTIALS: dict[str, tuple[bool, list[str], str, list[str], bool]] = {
    "same": (False, ["parent load"], "about:blank", [], True),
    "user": (False, ["parent load"], "about:blank", [], True),
    "redirected": (False, ["parent load"], "about:blank", ["/redirect?redirected referer http://A/redirected"], True),
    "location": (
        False,
        ["frame load f /child?location", "set location", "parent load"],
        "/child?location",
        ["/child?location referer http://A/location"],
        True,
    ),
    "named": (False, ["opened", "parent load"], "/child?named", ["/child?named referer http://A/named"], True),
    "samecreds": (
        True,
        ["frame load f /child?samecreds", "parent load"],
        "/child?samecreds",
        ["/child?samecreds referer http://A/samecreds"],
        False,
    ),
    "opened": (True, ["opened", "parent load"], "/opened", [], True),
}


class _Server(QuietServer):
    """Serves the cases, and keeps the children and redirects it was asked for."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        self.asked: list[str] = []
        self.release = threading.Event()
        self.a = f"127.0.0.1:{self.server_port}"
        self.b = f"localhost:{self.server_port}"

    def page(self, name: str) -> str:
        if name in _LAZY:
            return _LAZY[name][0]
        a, b = self.a, self.b
        return {
            "same": _frame("f", src=f"http://user:pass@{a}/child?same"),
            "user": _frame("f", src=f"http://user@{b}/child?user"),
            "redirected": _frame("f", src="/redirect?redirected"),
            "location": _frame("f", src="/child?location")
            + "<script>document.getElementById('f').addEventListener('load', () => {"
            f" document.getElementById('f').contentWindow.location.href = 'http://u:p@{a}/child?location2'; log('set location');"
            " }, {once: true});</script>",
            "named": "<iframe id=f name=target src='/child?named'></iframe><script>document.getElementById('f').addEventListener("
            f"'load', () => {{ open('http://u:p@{a}/child?named2', 'target'); log('opened'); }}, {{once: true}});</script>",
            "samecreds": _frame("f", src=f"http://u:p@{a}/child?samecreds"),
            "opened": "<iframe id=f></iframe><script>const d = document.getElementById('f').contentDocument; d.open();"
            f" d.write(`<iframe src='http://u:p@{a}/child?opened'"
            " onload=\"parent.postMessage('grandchild load', '*')\"></iframe>`);"
            " d.close(); log('opened');</script>",
        }[name]


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        assert isinstance(self.server, _Server)
        path, _, query = self.path.partition("?")
        if path in ("/child", "/inner", "/redirect"):
            referer = self.headers["Referer"]
            # The Referer, whole, with A for the server's host.
            self.server.asked.append(self.path + (f" referer {referer.replace(self.server.a, 'A')}" if referer else ""))
        if path == "/release":
            self.server.release.set()
        if path == "/redirect":
            self.send_response(302)
            self.send_header("Location", f"http://user:pass@{self.server.a}/child?{query}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/child" and query == "outer":
            body = (
                "<!doctype html><script>parent.postMessage('child ran ?outer', '*');</script>"
                "<iframe loading=lazy src='/child?deep' onload=\"parent.postMessage('deep load', '*')\"></iframe>"
            )
        elif path == "/child":
            body = _CHILD
        elif path == "/inner":
            body = "<!doctype html><iframe loading=lazy src='/child?innerchild'></iframe>"
        elif path == "/release":
            body = ""
        else:
            body = _HEAD + self.server.page(path[1:])
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        first, gate, rest = data.partition(_GATE.encode())
        self.wfile.write(first)
        self.wfile.flush()
        if gate:
            self.server.release.wait(_GATE_SECONDS)
        self.wfile.write(gate + rest)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@contextlib.contextmanager
def _page_of(name: str, *, credentials: bool = False) -> Generator[tuple[Any, _Server]]:
    """Open the case `name`, at a URL with u:p if `credentials`, and give its Page and the server."""
    server = _Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://{'u:p@' if credentials else ''}{server.a}/{name}"
    try:
        with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
            yield page, server
    finally:
        server.release.set()
        server.shutdown()
        server.server_close()


def _said_once_settled(page: Any, count: int) -> list[str]:
    """What the page has logged once it has logged `count` lines, and a moment more for any that should not come, but what its children
    say they ran, which comes before or after their frames' load events as their own loads end."""
    script = "said.filter((line) => !line.startsWith('child ran '))"
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while len(page.evaluate(script, timeout=_TIMEOUT_SECONDS)) < count:
        assert time.monotonic() < deadline, page.evaluate("said", timeout=_TIMEOUT_SECONDS)
        time.sleep(0.02)
    time.sleep(_GRACE_SECONDS)
    said: list[str] = page.evaluate(script, timeout=_TIMEOUT_SECONDS)
    return said


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("name", list(_LAZY))
def test_a_lazy_frame_loads_as_chrome_s_does(name: str) -> None:
    _, said, asked = _LAZY[name]
    with _page_of(name) as (page, server):
        assert _said_once_settled(page, len(said)) == said
        assert server.asked == asked


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("name", list(_CREDENTIALS))
def test_a_frame_goes_to_a_url_with_credentials_as_chrome_lets_it(name: str) -> None:
    credentials, said, at_end, asked, warned = _CREDENTIALS[name]
    with _page_of(name, credentials=credentials) as (page, server):
        # DOMContentLoaded comes in each, where Chrome's recording did not log it.
        logged = [line for line in _said_once_settled(page, len(said) + 1) if line != "DOMContentLoaded"]
        assert logged == said
        assert page.evaluate("where(document.getElementById('f'))", timeout=_TIMEOUT_SECONDS) == at_end
        assert server.asked == asked
        assert [message.text for message in page.console_messages] == ([_WARNING] if warned else [])
