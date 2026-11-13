"""The Recording Cases of a Script Fingerprint: the pages that run the probe in a real Chrome and post what it records.

The first Recording Case is a top-level page, an HTTPS page at the case pages' name, as #207 chose. The others run the
same page in a frame of the same origin, in a srcdoc frame and in a frame of another site, and the probe in a dedicated
worker that a top-level page starts. docs/capture.md, "The Script Fingerprint", says what each page does and why.
"""

import functools
import html
import json
import logging
from collections.abc import Mapping
from dataclasses import dataclass, field
from urllib.parse import unquote

from tools.capture.cases import CASE_HOST, CROSS_SITE_HOST
from tools.capture.http1 import Http1Request
from tools.capture.page import RequestHead, Response
from tools.capture.probe import RecordValue, probe_script
from tools.capture.recording import SCRIPT, Recording
from tools.capture.script_normal_form import (
    FRAME_MACHINE_PLACES,
    FRAME_VIEWPORT,
    MACHINE_PLACES,
    WORKER_MACHINE_PLACES,
    Place,
)

SCRIPT_DIRECTORY = "script"
"""Where a recording session writes the Script Fingerprint's files, apart from the wire's, and where its Baselines go
under tests/baselines/RELEASE/, which the wire's Parity Checks do not read."""

PAGE_PATH = "/script/page"
"""The page of the first Recording Case."""

RECORD_PATH = "/script/record"
"""Where the page posts the probe's record, once the probe has finished."""

_SAME_ORIGIN_FRAME_PATH = "/script/same-origin-frame"
_SRCDOC_FRAME_PATH = "/script/srcdoc-frame"
_CROSS_SITE_FRAME_PATH = "/script/cross-site-frame"
_WORKER_PATH = "/script/worker"
_FRAME = "/frame"
_WORKER_SCRIPT = "/worker.js"
_RECORD = "/record"
_FAILED_PREFIX = "/script/failed?"
_FRAME_BUDGET = 50_000

_HTML = "text/html; charset=utf-8"
_TEXT = "text/plain; charset=utf-8"
_JAVASCRIPT = "text/javascript; charset=utf-8"

_log = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class ScriptRun:
    """One start of a Chrome at a page that runs the probe, and the files that hold what it records.

    Attributes:
        name: The files' name, without .json and relative to the output directory.
        label: What to call it to a person.
        route: The route into the server whose recording it reads.
        host: The name it starts at.
        start_path: The path it starts at, the top-level page.
        done_path: The path the probe's page or worker posts its record to, which says that it has finished.
        machine_places: Each place whose value follows the machine where the probe runs, which the Normal Form hides.
        budget: How many bytes its Baseline may take for each Chrome Release, which the owner chose: 1.5 MB for the
            first case's whole Normal Form on 2026-11-12, and on 2026-11-13 50 KB for what a frame's differs in, and
            250 KB for what a worker's differs in.
        in_a_page: Whether a Page can run it, so that a Parity Check compares a Page with its Baselines. A Page has no
            frames and no workers yet.
        own_values: What the probe gives at the places of a top-level page's machine that the case decides instead,
            such as a frame's size, which the Normal Form keeps.
    """

    name: str
    label: str
    route: str
    host: str
    start_path: str
    done_path: str
    machine_places: tuple[Place, ...] = MACHINE_PLACES
    budget: int = 1_500_000
    in_a_page: bool = True
    own_values: Mapping[Place, RecordValue] = field(default_factory=dict[Place, RecordValue], hash=False)

    @property
    def case(self) -> str:
        """The Recording Case, the name of its Baseline's file in tests/baselines/RELEASE/script/, such as "page"."""
        return self.name.removeprefix(f"{SCRIPT_DIRECTORY}/")


TOP_LEVEL_PAGE = ScriptRun(
    f"{SCRIPT_DIRECTORY}/page", "Script Fingerprint of a top-level page", SCRIPT, CASE_HOST, PAGE_PATH, RECORD_PATH
)
"""The first Recording Case of a Script Fingerprint, whose Baseline holds the whole Normal Form. The Baselines of the
others hold only what differs from it."""

SAME_ORIGIN_FRAME = ScriptRun(
    f"{SCRIPT_DIRECTORY}/same_origin_frame",
    "Script Fingerprint of a frame of the same origin",
    SCRIPT,
    CASE_HOST,
    _SAME_ORIGIN_FRAME_PATH,
    _SAME_ORIGIN_FRAME_PATH + _RECORD,
    FRAME_MACHINE_PLACES,
    _FRAME_BUDGET,
    in_a_page=False,
    own_values=FRAME_VIEWPORT,
)
"""The probe in a frame of a top-level page, at the page's own origin."""

SRCDOC_FRAME = ScriptRun(
    f"{SCRIPT_DIRECTORY}/srcdoc_frame",
    "Script Fingerprint of a srcdoc frame",
    SCRIPT,
    CASE_HOST,
    _SRCDOC_FRAME_PATH,
    _SRCDOC_FRAME_PATH + _RECORD,
    FRAME_MACHINE_PLACES,
    _FRAME_BUDGET,
    in_a_page=False,
    own_values=FRAME_VIEWPORT,
)
"""The probe in a frame whose document its element holds, about:srcdoc, which has the page's origin and makes no request,
as a frame that a script writes into has."""

CROSS_SITE_FRAME = ScriptRun(
    f"{SCRIPT_DIRECTORY}/cross_site_frame",
    "Script Fingerprint of a frame of another site",
    SCRIPT,
    CASE_HOST,
    _CROSS_SITE_FRAME_PATH,
    _CROSS_SITE_FRAME_PATH + _RECORD,
    FRAME_MACHINE_PLACES,
    _FRAME_BUDGET,
    in_a_page=False,
    own_values=FRAME_VIEWPORT,
)
"""The probe in a frame of a top-level page, at a name of another site, which Chrome runs in a process of its own."""

DEDICATED_WORKER = ScriptRun(
    f"{SCRIPT_DIRECTORY}/worker",
    "Script Fingerprint of a dedicated worker",
    SCRIPT,
    CASE_HOST,
    _WORKER_PATH,
    _WORKER_PATH + _RECORD,
    WORKER_MACHINE_PLACES,
    250_000,
    in_a_page=False,
)
"""The probe in a dedicated worker that a top-level page starts."""

SCRIPT_RUNS = (TOP_LEVEL_PAGE, SAME_ORIGIN_FRAME, SRCDOC_FRAME, CROSS_SITE_FRAME, DEDICATED_WORKER)
"""Every start of a Chrome that records a Script Fingerprint, in the order a recording session takes them, the first
Recording Case first."""

SCRIPT_DONE_PATHS = frozenset(run.done_path for run in SCRIPT_RUNS)


def posted_record(recording: Recording, run: ScriptRun) -> bytes | None:
    """Return what a run's page posted to its done path, the probe's record, or None if it posted nothing there."""
    for connection in recording.connections:
        for request in connection.requests:
            if request.target == run.done_path and isinstance(request, Http1Request):
                return request.body
    return None


def _probe_source() -> str:
    # The probe goes in as a string, which is evaluated as the Page program evaluates it. "<" goes escaped, so that
    # nothing in the probe ends the script of a page.
    return json.dumps(probe_script()).replace("<", "\\u003c")


def _probe_page(record_path: str) -> bytes:
    # The page evaluates the probe only after the load event, so that no event is being dispatched while it reads the
    # window. The page sets nothing on the window and gives no element an id or a name, which would be properties of the
    # window, and it uses no regular expression, whose matches would change RegExp's legacy statics.
    return _page_that_runs(
        f"""  addEventListener("load", () => {{
    setTimeout(() => {{
{_run_probe(record_path, "      ")}
    }});
  }});"""
    )


def _probe_frame(record_path: str) -> bytes:
    # A frame runs the top-level page's markup and probe, so that what differs from the top-level page is what being a
    # frame changes. A frame of another site, which Chrome runs in a process of its own, gets its size from the browser
    # a moment after it has loaded, so the probe waits for it, for at most five seconds.
    return _page_that_runs(
        f"""  addEventListener("load", () => {{
    const deadline = Date.now() + 5000;
    const sized = () => {{
      if (innerWidth === 0 && Date.now() < deadline) {{
        setTimeout(sized, 20);
        return;
      }}
{_run_probe(record_path, "      ")}
    }};
    setTimeout(sized);
  }});"""
    )


def _run_probe(record_path: str, indent: str) -> str:
    lines = f"""const progress = document.querySelector("p");
(0, eval)({_probe_source()})
  .then((record) => fetch("{record_path}", {{method: "POST", body: record}}))
  .then(() => {{ progress.textContent = "Recorded. You can close this window."; }})
  .catch((error) => {{
    progress.textContent = "Failed: " + error;
    fetch("{_FAILED_PREFIX}" + encodeURIComponent(error));
  }});"""
    return "\n".join(indent + line for line in lines.splitlines())


def _page_that_runs(script: str) -> bytes:
    return f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<p>Recording...</p>
<script>
{script}
</script>
</html>
""".encode()


def _frame_parent(attribute: str) -> bytes:
    # The frame takes the size that an iframe has by default, 300 by 150 pixels, and nothing else of the parent's.
    return f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<iframe {attribute}></iframe>
</html>
""".encode()


def _src(url: str) -> str:
    return f'src="{url}"'


def _srcdoc(document: bytes) -> str:
    return f'srcdoc="{html.escape(document.decode(), quote=True)}"'


def _worker_parent() -> bytes:
    # The worker says when it has finished, and an error in its script reaches the page.
    worker_url = _WORKER_PATH + _WORKER_SCRIPT
    return f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<p>Recording...</p>
<script>
  const progress = document.querySelector("p");
  const worker = new Worker("{worker_url}");
  worker.onmessage = (event) => {{ progress.textContent = event.data; }};
  worker.onerror = (event) => {{
    progress.textContent = "Failed: " + event.message;
    fetch("{_FAILED_PREFIX}" + encodeURIComponent(event.message));
  }};
</script>
</html>
""".encode()


def _worker_script() -> bytes:
    # The script declares nothing, which would be a property of the worker's global object, and it runs the probe in a
    # task of its own, as the page does once its load event is over.
    record_path = DEDICATED_WORKER.done_path
    return f"""setTimeout(() => {{
  (0, eval)({_probe_source()})
    .then((record) => fetch("{record_path}", {{method: "POST", body: record}}))
    .then(() => {{ postMessage("Recorded. You can close this window."); }})
    .catch((error) => {{
      postMessage("Failed: " + error);
      fetch("{_FAILED_PREFIX}" + encodeURIComponent(error));
    }});
}});
""".encode()


_RECORDED = Response(200, _TEXT, b"recorded\n")
_NOT_FOUND = Response(404, _TEXT, b"not found\n")


@functools.lru_cache(maxsize=8)
def _responses(port: int) -> dict[str, Response]:
    cross_site_frame = f"https://{CROSS_SITE_HOST}:{port}{_CROSS_SITE_FRAME_PATH}{_FRAME}"
    return {
        PAGE_PATH: Response(200, _HTML, _probe_page(RECORD_PATH)),
        _SAME_ORIGIN_FRAME_PATH: Response(200, _HTML, _frame_parent(_src(_SAME_ORIGIN_FRAME_PATH + _FRAME))),
        _SAME_ORIGIN_FRAME_PATH + _FRAME: Response(200, _HTML, _probe_frame(SAME_ORIGIN_FRAME.done_path)),
        _SRCDOC_FRAME_PATH: Response(200, _HTML, _frame_parent(_srcdoc(_probe_frame(SRCDOC_FRAME.done_path)))),
        _CROSS_SITE_FRAME_PATH: Response(200, _HTML, _frame_parent(_src(cross_site_frame))),
        _CROSS_SITE_FRAME_PATH + _FRAME: Response(200, _HTML, _probe_frame(CROSS_SITE_FRAME.done_path)),
        _WORKER_PATH: Response(200, _HTML, _worker_parent()),
        _WORKER_PATH + _WORKER_SCRIPT: Response(200, _JAVASCRIPT, _worker_script()),
        **{run.done_path: _RECORDED for run in SCRIPT_RUNS},
    }


def respond(port: int, request: RequestHead) -> Response:
    """Answer a request on the route of the Script Fingerprint's pages, whose port a frame of another site names."""
    target = request.target
    if target.startswith(_FAILED_PREFIX):
        # This is the only trace of what went wrong in a Chrome that nobody is looking at.
        _log.warning("the page failed: %s", unquote(target.removeprefix(_FAILED_PREFIX)))
        return Response(200, _TEXT, b"noted\n")
    return _responses(port).get(target, _NOT_FOUND)
