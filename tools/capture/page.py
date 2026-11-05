"""The test page and the other paths the capture server answers, one for each main Request Kind."""

import base64
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum


class RequestKind(StrEnum):
    NAVIGATION = "navigation"
    FETCH = "fetch"
    SCRIPT = "script"
    IMAGE = "image"


@dataclass(frozen=True, slots=True)
class Response:
    status: int
    content_type: str
    body: bytes


type Responder = Callable[[str], Response]
"""What answers a request, given its target."""

KIND_PATHS = {
    RequestKind.NAVIGATION: "/",
    RequestKind.FETCH: "/fetch",
    RequestKind.SCRIPT: "/script.js",
    RequestKind.IMAGE: "/image.png",
}
"""The path that stands for each Request Kind. A request to any other path counts as no Request Kind."""

DONE_PATH = "/done"
"""The test page requests this once the requests for the other Request Kinds have finished."""

# The data: icon keeps Chrome from asking for /favicon.ico, which is no Request Kind of ours.
_PAGE = f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
<script src="{KIND_PATHS[RequestKind.SCRIPT]}"></script>
<p id="progress">Recording...</p>
<img id="image" src="{KIND_PATHS[RequestKind.IMAGE]}" width="1" height="1" alt="">
<script>
  const progress = document.getElementById("progress");
  const image = document.getElementById("image");
  const imageLoaded = image.complete
    ? Promise.resolve()
    : new Promise((resolve, reject) => {{
        image.onload = resolve;
        image.onerror = () => reject(new Error("the image failed to load"));
      }});
  Promise.all([imageLoaded, fetch("{KIND_PATHS[RequestKind.FETCH]}").then((response) => response.text())])
    .then(() => fetch("{DONE_PATH}"))
    .then(() => {{ progress.textContent = "Recorded. You can close this window."; }})
    .catch((error) => {{ progress.textContent = "Failed: " + error; }});
</script>
</html>
""".encode()

_IMAGE = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII="
)

_RESPONSES = {
    KIND_PATHS[RequestKind.NAVIGATION]: Response(200, "text/html; charset=utf-8", _PAGE),
    KIND_PATHS[RequestKind.FETCH]: Response(200, "text/plain; charset=utf-8", b"fetched\n"),
    KIND_PATHS[RequestKind.SCRIPT]: Response(200, "text/javascript; charset=utf-8", b"// Nothing to do.\n"),
    KIND_PATHS[RequestKind.IMAGE]: Response(200, "image/png", _IMAGE),
    DONE_PATH: Response(200, "text/plain; charset=utf-8", b"done\n"),
}
_NOT_FOUND = Response(404, "text/plain; charset=utf-8", b"not found\n")


def respond(target: str) -> Response:
    """Answer a request for target."""
    return _RESPONSES.get(target, _NOT_FOUND)


def kind_of(target: str) -> RequestKind | None:
    """Return the Request Kind a request for target stands for, if any."""
    for kind, path in KIND_PATHS.items():
        if path == target:
            return kind
    return None
