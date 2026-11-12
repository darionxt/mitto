"""The Recording Cases of a Script Fingerprint: the page that runs the probe in a real Chrome and posts what it records.

The first Recording Case is a top-level page, an HTTPS page at the case pages' name, as #207 chose. Frames and workers
come with M8. docs/capture.md, "The Script Fingerprint", says what the page does and why.
"""

import json
import logging
from dataclasses import dataclass
from urllib.parse import unquote

from tools.capture.cases import CASE_HOST
from tools.capture.http1 import Http1Request
from tools.capture.page import RequestHead, Response
from tools.capture.probe import probe_script
from tools.capture.recording import SCRIPT, Recording

SCRIPT_DIRECTORY = "script"
"""Where a recording session writes the Script Fingerprint's files, apart from the wire's, and where its Baselines go
under tests/baselines/RELEASE/, which the wire's Parity Checks do not read."""

PAGE_PATH = "/script/page"
"""The page of the first Recording Case."""

RECORD_PATH = "/script/record"
"""Where the page posts the probe's record, once the probe has finished."""

_FAILED_PREFIX = "/script/failed?"

_HTML = "text/html; charset=utf-8"
_TEXT = "text/plain; charset=utf-8"

_log = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class ScriptRun:
    """One start of a Chrome at a page that runs the probe, and the files that hold what it records.

    Attributes:
        name: The files' name, without .json and relative to the output directory.
        label: What to call it to a person.
        route: The route into the server whose recording it reads.
        host: The name it starts at.
        start_path: The path it starts at.
        done_path: The path the page posts its record to, which says that it has finished.
    """

    name: str
    label: str
    route: str
    host: str
    start_path: str
    done_path: str


TOP_LEVEL_PAGE = ScriptRun(
    f"{SCRIPT_DIRECTORY}/page", "Script Fingerprint of a top-level page", SCRIPT, CASE_HOST, PAGE_PATH, RECORD_PATH
)
"""The first Recording Case of a Script Fingerprint."""

SCRIPT_RUNS = (TOP_LEVEL_PAGE,)
"""Every start of a Chrome that records a Script Fingerprint, in the order a recording session takes them."""

SCRIPT_DONE_PATHS = frozenset(run.done_path for run in SCRIPT_RUNS)


def posted_record(recording: Recording, run: ScriptRun) -> bytes | None:
    """Return what a run's page posted to its done path, the probe's record, or None if it posted nothing there."""
    for connection in recording.connections:
        for request in connection.requests:
            if request.target == run.done_path and isinstance(request, Http1Request):
                return request.body
    return None


def _page() -> bytes:
    # The probe goes in the page as a string, which the page evaluates as the Page program does, and only after the load
    # event, so that no event is being dispatched while it reads the window. The page sets nothing on the window and
    # gives no element an id or a name, which would be properties of the window, and it uses no regular expression,
    # whose matches would change RegExp's legacy statics. "<" goes escaped, so that nothing in the probe ends the
    # script.
    source = json.dumps(probe_script()).replace("<", "\\u003c")
    return f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<p>Recording...</p>
<script>
  addEventListener("load", () => {{
    setTimeout(() => {{
      const progress = document.querySelector("p");
      (0, eval)({source})
        .then((record) => fetch("{RECORD_PATH}", {{method: "POST", body: record}}))
        .then(() => {{ progress.textContent = "Recorded. You can close this window."; }})
        .catch((error) => {{
          progress.textContent = "Failed: " + error;
          fetch("{_FAILED_PREFIX}" + encodeURIComponent(error));
        }});
    }});
  }});
</script>
</html>
""".encode()


_NOT_FOUND = Response(404, _TEXT, b"not found\n")


def respond(request: RequestHead) -> Response:
    """Answer a request on the route of the Script Fingerprint's pages."""
    target = request.target
    if target == PAGE_PATH:
        return Response(200, _HTML, _page())
    if target == RECORD_PATH:
        return Response(200, _TEXT, b"recorded\n")
    if target.startswith(_FAILED_PREFIX):
        # This is the only trace of what went wrong in a Chrome that nobody is looking at.
        _log.warning("the page failed: %s", unquote(target.removeprefix(_FAILED_PREFIX)))
        return Response(200, _TEXT, b"noted\n")
    return _NOT_FOUND
