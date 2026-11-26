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
from urllib.parse import parse_qs, quote, urlsplit

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 120
_TESTS = Path(__file__).parent
_WORKER = (_TESTS / "worker_referrer_cases.js").read_text(encoding="utf-8")

POLICIES = ("", "no-referrer", "origin", "unsafe-url", "no-referrer, unknown", "no-referrer, v2")
"""The policies that a document's response and a worker's have, in each combination. A token that names no policy is left
out, and one that is not letters and hyphens takes the whole header with it."""

_PAGE = """<!doctype html><title>r</title><body><script>
const cases = (async () => {
  for (const type of ['classic', 'module']) {
    for (const policy of %s) {
      const id = encodeURIComponent(`${document.title}/${type}/${policy}`);
      const worker = new Worker(
        `/ask?case=${id}&what=script&script=1&worker=1&policy=${encodeURIComponent(policy)}`
          + `&static=${type === 'module' ? 1 : ''}`, {type});
      await new Promise((resolve) => { worker.onmessage = resolve; worker.onerror = resolve; });
      worker.terminate();
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
        # The URL of each document that the server gave, by its policy, and of each worker's script, by its case and
        # whether it is a nested worker's.
        self.documents: dict[str, str] = {}
        self.scripts: dict[str, dict[str, str]] = {}

    def took(self, query: dict[str, str], referrer: str | None) -> None:
        """Note the Referer of one thing that a worker asks for. Only the whole URL of the case's own document or of one
        of its workers' scripts gets a name; any other stays as it came, such as what a request asked for."""
        seen = referrer
        case = query["case"]
        names = {
            f"{self.origin}/": "origin",
            f"{self.origin}/other": "other",
            **{url: name for name, url in self.scripts.get(case, {}).items()},
        }
        document = self.documents.get(case.split("/")[0])
        if document is not None:
            names[document] = "document"
        if referrer is not None:
            seen = names.get(referrer, referrer)
        self.referrers.setdefault(case, {})[query["what"]] = seen


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        url = urlsplit(self.path)
        query = {name: values[0] for name, values in parse_qs(url.query).items()}
        policy = query.get("policy", "")
        assert isinstance(self.server, ReferrerServer)
        if url.path != "/ask":
            if url.path == "/":
                self.server.documents[policy] = self.server.origin + self.path
            body = (_PAGE % json.dumps(POLICIES)).replace("<title>r", f"<title>{policy}").encode()
            self._answer(body, "text/html; charset=utf-8", policy)
            return
        self.server.took(query, self.headers.get("Referer"))
        if "worker" in query:
            name = "nested worker" if "nested" in query else "worker"
            self.server.scripts.setdefault(query["case"], {})[name] = self.server.origin + self.path
            body = _WORKER.encode()
            if "static" in query:
                asked = f"/ask?case={quote(query['case'], safe='')}&what=static%20import&script=1"
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
        session._open_page(f"{server.origin}/?policy={quote(policy)}", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate("cases", timeout=_TIMEOUT_SECONDS) == "done"

    assert server.referrers == {case: seen for case, seen in expected.items() if case.split("/")[0] == policy}
