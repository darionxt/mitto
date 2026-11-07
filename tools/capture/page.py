"""The test page and the other paths the capture server answers, one for each main Request Kind."""

import base64
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum
from typing import Protocol


class RequestKind(StrEnum):
    NAVIGATION = "navigation"
    FETCH = "fetch"
    SCRIPT = "script"
    IMAGE = "image"


class RequestHead(Protocol):
    """What the three protocols' requests have in common, which is all it takes to answer one."""

    @property
    def target(self) -> str: ...

    @property
    def headers(self) -> tuple[tuple[str, str], ...]: ...


@dataclass(frozen=True, slots=True)
class Response:
    """An answer.

    Attributes:
        status: The status code.
        content_type: The value of Content-Type.
        body: The body.
        cache_control: The value of Cache-Control.
        headers: More (name, value) pairs to send, in order, with the case HTTP/1.1 uses.
        close: Whether the connection ends with this answer, so that the client's next request needs a new one.
    """

    status: int
    content_type: str
    body: bytes
    cache_control: str = "no-store"
    headers: tuple[tuple[str, str], ...] = ()
    close: bool = False


type Responder = Callable[[RequestHead], Response]
"""What answers a request."""

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


def response_for(target: str) -> Response:
    """Answer a request for target, as the test page and its paths do."""
    return _RESPONSES.get(target, _NOT_FOUND)


def respond(request: RequestHead) -> Response:
    """Answer a request, as the test page and its paths do."""
    return response_for(request.target)
