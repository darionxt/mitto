"""Tests of the Referer of what dedicated workers ask for, on a local server.

A worker's script goes with its creator's referrer policy, and what the worker asks for with that of its script's
response. tests/data/worker_referrer_cases.json keeps what Chrome 157 sent for the same pages and workers, headless on
macOS (157.0.8103.79).
"""

import http.server
import json
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlsplit

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_WORKER = (_TESTS / "worker_referrer_cases.js").read_text(encoding="utf-8")

POLICIES = ("", "no-referrer", "origin", "unsafe-url")
"""The policies that a document's response and a worker's have, in each combination."""

_PAGE = """<!doctype html><title>r</title><body><script>
const cases = (async () => {
  for (const type of ['classic', 'module']) {
    for (const policy of %s) {
      const id = encodeURIComponent(`${document.title}/${type}/${policy}`);
      const worker = new Worker(
        `/ask?case=${id}&what=script&script=1&worker=1&policy=${policy}&static=${type === 'module' ? 1 : ''}`, {type});
      await new Promise((resolve) => { worker.onmessage = resolve; worker.onerror = resolve; });
    }
  }
  return 'done';
})();
</script>"""


class ReferrerServer(QuietServer):
    """A server of the page at `/?policy=`, whose workers ask at `/ask`, with the Referer of each thing that they ask
    for, by its case and then what it is: "document", "worker", "origin" or "other" for what it names, or None."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        self.referrers: dict[str, dict[str, str | None]] = {}
        self.origin = f"http://127.0.0.1:{self.server_port}"

    def took(self, query: dict[str, str], referrer: str | None) -> None:
        """Note the Referer of one thing that a worker asks for."""
        seen = referrer
        if referrer is not None:
            path = urlsplit(referrer).path
            if referrer == f"{self.origin}/":
                seen = "origin"
            elif path == "/":
                seen = "document"
            elif path == "/ask":
                asker = parse_qs(urlsplit(referrer).query)
                seen = "nested worker" if "nested" in asker else "worker"
            elif path == "/other":
                seen = "other"
        self.referrers.setdefault(query["case"], {})[query["what"]] = seen


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        url = urlsplit(self.path)
        query = {name: values[0] for name, values in parse_qs(url.query).items()}
        policy = query.get("policy", "")
        if url.path != "/ask":
            body = (_PAGE % json.dumps(POLICIES)).replace("<title>r", f"<title>{policy}").encode()
            self._answer(body, "text/html; charset=utf-8", policy)
            return
        assert isinstance(self.server, ReferrerServer)
        self.server.took(query, self.headers.get("Referer"))
        if "worker" in query:
            body = _WORKER.encode()
            if "static" in query:
                asked = f"/ask?case={query['case'].replace('/', '%2F')}&what=static%20import&script=1"
                body = f"import '{asked}';\n".encode() + body
            self._answer(body, "text/javascript; charset=utf-8", policy)
            return
        self._answer(b"", "text/javascript" if "script" in query else "text/plain", "")

    def _answer(self, body: bytes, content_type: str, policy: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if policy:
            self.send_header("Referrer-Policy", policy)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[ReferrerServer]:
    server = ReferrerServer()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("policy", POLICIES, ids=lambda policy: policy or "none")
def test_what_a_worker_asks_for_is_referred_as_chrome_refers_it(server: ReferrerServer, policy: str) -> None:
    expected = json.loads((_TESTS / "data" / "worker_referrer_cases.json").read_text(encoding="utf-8"))

    with (
        mitto.Session() as session,
        session._open_page(f"{server.origin}/?policy={policy}", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate("cases", timeout=_TIMEOUT_SECONDS) == "done"

    assert server.referrers == {case: seen for case, seen in expected.items() if case.split("/")[0] == policy}
