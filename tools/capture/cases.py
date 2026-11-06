"""The Recording Cases, the pages that make a real Chrome produce them, and the starts of Chrome that go through them."""

import functools
import json
import logging
from dataclasses import dataclass
from urllib.parse import unquote

from tools.capture.page import (
    DONE_PATH,
    KIND_PATHS,
    RequestHead,
    RequestKind,
    Response,
    response_for,
)
from tools.capture.recording import HTTP1, HTTP2, HTTP3, PLAIN_HTTP, PROXIED_HTTP1, PROXIED_HTTP2

CASE_HOST = "capture.test"
"""The name the case pages are at. Unlike a *.localhost name it is no local host, so it counts as a host on a network:
a proxy applies, and plain HTTP is not potentially trustworthy. The name comes from RFC 2606, so nothing else has it."""

SAME_SITE_HOST = "same-site.capture.test"
"""Another origin, but the same site as CASE_HOST."""

CROSS_SITE_HOST = "cross-site.test"
"""Another site than CASE_HOST."""

HOST_NAMES = (CASE_HOST, SAME_SITE_HOST, CROSS_SITE_HOST)

START_PATH = "/case/start"
_FORMS_PATH = "/case/forms"
_DONE_PATH = "/case/done"
_PLAIN_HTTP_DONE_PATH = "/case/plain-http-done"
_PROXY_DONE_PATH = "/case/proxy-done"
_FAILED_PREFIX = "/case/failed?"
_COOKIE_SET_PATH = "/case/cookie-set"
_COOKIE = "mitto=1"

_CASE_PREFIX = "/case/"
_TEXT = "text/plain; charset=utf-8"
_HTML = "text/html; charset=utf-8"
_JAVASCRIPT = "text/javascript; charset=utf-8"
_PNG = "image/png"
_EVERY_ORIGIN = ("Access-Control-Allow-Origin", "*")
_ETAG = '"mitto"'

_log = logging.getLogger(__name__)


def has_header(request: RequestHead, name: str) -> bool:
    """Return whether a request holds a header, given its name in lower case."""
    return any(sent.lower() == name for sent, _ in request.headers)


@dataclass(frozen=True, slots=True)
class Case:
    """Which request of a recording stands for one Recording Case.

    Attributes:
        name: Its key in a normal form's "requests".
        path: The path its request goes to.
        carries: A header its request must hold, in lower case. An earlier request to the path does not count.
        refused_path: The path the page asks for when Chrome refuses to send the request at all.
    """

    name: str
    path: str
    carries: str | None = None
    refused_path: str | None = None

    def matches(self, request: RequestHead) -> bool:
        """Return whether a request stands for this case."""
        return request.target == self.path and (self.carries is None or has_header(request, self.carries))


def _case(name: str, *, carries: str | None = None, refused_path: str | None = None) -> Case:
    return Case(name, _CASE_PREFIX + name.replace("_", "-"), carries, refused_path)


_LINK_SAME_ORIGIN = _case("navigation_link_same_origin")
_LINK_SAME_SITE = _case("navigation_link_same_site")
_LINK_CROSS_SITE = _case("navigation_link_cross_site")
_FORM_URLENCODED = _case("navigation_form_urlencoded")
_FORM_MULTIPART = _case("navigation_form_multipart")
_FETCH_GET_SAME_SITE = _case("fetch_get_same_site")
_FETCH_GET_CROSS_SITE = _case("fetch_get_cross_site")
_FETCH_POST_TEXT_SAME_ORIGIN = _case("fetch_post_text_same_origin")
_FETCH_POST_TEXT_CROSS_SITE = _case("fetch_post_text_cross_site")
_FETCH_POST_JSON_SAME_ORIGIN = _case("fetch_post_json_same_origin")
_FETCH_POST_STREAM_REFUSED_PATH = "/case/fetch-post-stream-refused"
_FETCH_POST_STREAM = _case("fetch_post_stream", refused_path=_FETCH_POST_STREAM_REFUSED_PATH)
_SCRIPT_SAME_SITE = _case("script_same_site")
_SCRIPT_CROSS_SITE = _case("script_cross_site")
_IMAGE_SAME_SITE = _case("image_same_site")
_IMAGE_CROSS_SITE = _case("image_cross_site")
_COOKIE_CARRIED = _case("cookie", carries="cookie")
_REVALIDATION = _case("revalidation", carries="if-none-match")
_REDIRECT_NAVIGATION = tuple(_case(f"redirect_navigation_{hop}") for hop in range(1, 5))
_REDIRECT_FETCH = tuple(_case(f"redirect_fetch_{hop}") for hop in range(1, 5))
_PLAIN_HTTP_NAVIGATION = _case("plain_http_navigation")
_PLAIN_HTTP_FETCH = _case("plain_http_fetch")
_PLAIN_HTTP_SCRIPT = _case("plain_http_script")
_PLAIN_HTTP_IMAGE = _case("plain_http_image")
_PROXY_NAVIGATION = _case("proxy_navigation")

PROXY_CONNECT = Case("proxy_connect", "<CONNECT>")
"""What opens a tunnel through the proxy. Its path is one no request has, because CONNECT names a server instead."""

MAIN_CASES = tuple(Case(kind.value, KIND_PATHS[kind]) for kind in RequestKind)
"""The main Request Kinds, from one same-origin Initiator."""

_TLS_CASES = (
    _LINK_SAME_ORIGIN,
    _LINK_SAME_SITE,
    _LINK_CROSS_SITE,
    _FORM_URLENCODED,
    _FORM_MULTIPART,
    _FETCH_GET_SAME_SITE,
    _FETCH_GET_CROSS_SITE,
    _FETCH_POST_TEXT_SAME_ORIGIN,
    _FETCH_POST_TEXT_CROSS_SITE,
    _FETCH_POST_JSON_SAME_ORIGIN,
    _FETCH_POST_STREAM,
    _SCRIPT_SAME_SITE,
    _SCRIPT_CROSS_SITE,
    _IMAGE_SAME_SITE,
    _IMAGE_CROSS_SITE,
    _COOKIE_CARRIED,
    _REVALIDATION,
    *_REDIRECT_NAVIGATION,
    *_REDIRECT_FETCH,
)
_PLAIN_HTTP_CASES = (_PLAIN_HTTP_NAVIGATION, _PLAIN_HTTP_FETCH, _PLAIN_HTTP_SCRIPT, _PLAIN_HTTP_IMAGE)
_PROXY_CASES = (PROXY_CONNECT, _PROXY_NAVIGATION)

CASES = (*MAIN_CASES, *_TLS_CASES, *_PLAIN_HTTP_CASES, *_PROXY_CASES)
"""Every Recording Case, in the order a normal form lists them."""


@dataclass(frozen=True, slots=True)
class Run:
    """One start of a Chrome: where it goes, what makes it finish, and which file holds what it sent.

    Attributes:
        name: The recording's file name, without .json and relative to the output directory.
        label: What to call it to a person.
        route: The route into the server whose recording it reads.
        host: The name it starts at, or nothing for the name the server was given.
        start_path: The path it starts at.
        done_path: The path the page asks for when it has finished.
        cases: The Recording Cases it produces.
    """

    name: str
    label: str
    route: str
    host: str | None
    start_path: str
    done_path: str
    cases: tuple[Case, ...]


def _main_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, None, KIND_PATHS[RequestKind.NAVIGATION], DONE_PATH, MAIN_CASES)


def _tls_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, START_PATH, _DONE_PATH, _TLS_CASES)


def _proxy_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, _PROXY_NAVIGATION.path, _PROXY_DONE_PATH, _PROXY_CASES)


RUNS = (
    _main_run("http1", "HTTP/1.1", HTTP1),
    _main_run("http2", "HTTP/2", HTTP2),
    _main_run("http3", "HTTP/3", HTTP3),
    _tls_run("cases/http1", "Cases over HTTP/1.1", HTTP1),
    _tls_run("cases/http2", "Cases over HTTP/2", HTTP2),
    _tls_run("cases/http3", "Cases over HTTP/3", HTTP3),
    Run(
        "cases/plain_http",
        "Cases over plain HTTP",
        PLAIN_HTTP,
        CASE_HOST,
        _PLAIN_HTTP_NAVIGATION.path,
        _PLAIN_HTTP_DONE_PATH,
        _PLAIN_HTTP_CASES,
    ),
    _proxy_run("cases/proxy_http1", "Cases through a proxy, over HTTP/1.1", PROXIED_HTTP1),
    _proxy_run("cases/proxy_http2", "Cases through a proxy, over HTTP/2", PROXIED_HTTP2),
)
"""Every start of a Chrome that a recording session takes, in the order it takes them."""

DONE_PATHS = frozenset(run.done_path for run in RUNS)

_PAGE_SCRIPT = f"""
  const progress = document.getElementById("progress");
  const next = document.getElementById("next");
  const report = (error) => {{
    progress.textContent = "Failed: " + error;
    fetch("{_FAILED_PREFIX}" + encodeURIComponent(error));
  }};
  const get = (url, init) => fetch(url, init).then((response) => response.text());
  const showNext = () => {{
    progress.textContent = "Click below to go on.";
    next.hidden = false;
  }};
"""

_START_SCRIPT = """
  const loaded = (image) => image.complete
    ? Promise.resolve()
    : new Promise((resolve, reject) => {
        image.onload = resolve;
        image.onerror = () => reject(new Error(image.src + " failed to load"));
      });
  const streamBody = () => new ReadableStream({
    start(controller) {
      controller.enqueue(new TextEncoder().encode("stream body"));
      controller.close();
    },
  });
  (async () => {
    await Promise.all([...document.images].map(loaded));
    await get(urls.fetch_get_same_site);
    await get(urls.fetch_get_cross_site);
    await get(urls.fetch_post_text_same_origin, {method: "POST", body: "text body"});
    await get(urls.fetch_post_text_cross_site, {method: "POST", body: "text body"});
    await get(urls.fetch_post_json_same_origin, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: '{"json":"body"}',
    });
    // Chrome sends a stream body over HTTP/2 and HTTP/3 only. Over HTTP/1.1 it fails with no request on the wire.
    await get(urls.fetch_post_stream, {method: "POST", duplex: "half", body: streamBody()})
      .catch(() => get(urls.fetch_post_stream_refused));
    await get(urls.cookie_set);
    await get(urls.cookie);
    await get(urls.revalidation);
    await get(urls.revalidation);
    await get(urls.redirect_fetch_1);
    showNext();
  })().catch(report);
"""

_SHOW_NEXT_ON_LOAD = """
  addEventListener("load", showNext);
"""

_FILL_FILE_INPUT = """
  const files = new DataTransfer();
  files.items.add(new File(["file text\\n"], 'file "name".txt', {type: "text/plain"}));
  document.getElementById("file").files = files.files;
"""


def _report_done(path: str) -> str:
    return f"""
  get("{path}")
    .then(() => {{ progress.textContent = "Recorded. You can close this window."; }})
    .catch(report);
"""


def _document(body: str) -> bytes:
    # The data: icon keeps Chrome from asking for /favicon.ico, which is no case of ours.
    return f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="icon" href="data:,">
{body}
</html>
""".encode()


def _page(*parts: str, script: str = _SHOW_NEXT_ON_LOAD) -> Response:
    """Return a page that shows a progress line, holds parts, and runs script once it has what parts define."""
    body = "\n".join(('<p id="progress">Recording...</p>', *parts, f"<script>{_PAGE_SCRIPT}{script}</script>"))
    return _case_response(_HTML, _document(body))


def _case_response(
    content_type: str, body: bytes, *headers: tuple[str, str], cache_control: str = "no-store"
) -> Response:
    # Everything answers every origin, because what a page asks of another origin only works if the answer allows it,
    # and a request carries no sign of that.
    return Response(200, content_type, body, cache_control, (_EVERY_ORIGIN, *headers))


def _link_page(to: str) -> Response:
    return _page(f'<p><a id="next" hidden href="{to}">Click this link.</a></p>')


def _redirect(to: str) -> Response:
    return Response(302, _TEXT, b"", headers=(_EVERY_ORIGIN, ("Location", to)))


_SUBMIT = '<button id="next" hidden>Click this button.</button>'
_URLENCODED_FIELDS = (
    '<input type="hidden" name="name" value="mitto"><input type="hidden" name="a b&amp;c" value="d e&amp;f+g">'
)
_MULTIPART_FIELDS = (
    '<input type="hidden" name="name" value="mitto">'
    '<input type="hidden" name="a &quot;b&quot;" value="d e&amp;f+g">'
    '<input id="file" type="file" name="file">'
)


def _start_page(page: str, same_site: str, cross_site: str) -> Response:
    urls = {
        "fetch_get_same_site": same_site + _FETCH_GET_SAME_SITE.path,
        "fetch_get_cross_site": cross_site + _FETCH_GET_CROSS_SITE.path,
        "fetch_post_text_same_origin": _FETCH_POST_TEXT_SAME_ORIGIN.path,
        "fetch_post_text_cross_site": cross_site + _FETCH_POST_TEXT_CROSS_SITE.path,
        "fetch_post_json_same_origin": _FETCH_POST_JSON_SAME_ORIGIN.path,
        "fetch_post_stream": _FETCH_POST_STREAM.path,
        "fetch_post_stream_refused": _FETCH_POST_STREAM_REFUSED_PATH,
        "cookie_set": _COOKIE_SET_PATH,
        "cookie": _COOKIE_CARRIED.path,
        "revalidation": _REVALIDATION.path,
        "redirect_fetch_1": _REDIRECT_FETCH[0].path,
    }
    return _page(
        f'<script src="{same_site}{_SCRIPT_SAME_SITE.path}"></script>',
        f'<script src="{cross_site}{_SCRIPT_CROSS_SITE.path}"></script>',
        f'<img src="{same_site}{_IMAGE_SAME_SITE.path}" width="1" height="1" alt="">',
        f'<img src="{cross_site}{_IMAGE_CROSS_SITE.path}" width="1" height="1" alt="">',
        f'<p><a id="next" hidden href="{page}{_LINK_SAME_ORIGIN.path}">Click this link.</a></p>',
        f"<script>const urls = {json.dumps(urls)};</script>",
        script=_START_SCRIPT,
    )


def _plain_http_page() -> Response:
    return _page(
        f'<script src="{_PLAIN_HTTP_SCRIPT.path}"></script>',
        f'<img id="image" src="{_PLAIN_HTTP_IMAGE.path}" width="1" height="1" alt="">',
        script=f"""
  addEventListener("load", () => {{
    get("{_PLAIN_HTTP_FETCH.path}").then(() => {{ {_report_done(_PLAIN_HTTP_DONE_PATH)} }}).catch(report);
  }});
""",
    )


@functools.lru_cache(maxsize=64)
def _responses(scheme: str, port: int) -> dict[str, Response]:
    """Return the answer to each path of the case pages on a port. They name other origins, so they depend on it."""
    page = f"{scheme}://{CASE_HOST}:{port}"
    same_site = f"{scheme}://{SAME_SITE_HOST}:{port}"
    cross_site = f"{scheme}://{CROSS_SITE_HOST}:{port}"
    done = _case_response(_TEXT, b"done\n")
    script = _case_response(_JAVASCRIPT, b"// Nothing to do.\n")
    image = _case_response(_PNG, response_for(KIND_PATHS[RequestKind.IMAGE]).body)
    text = _case_response(_TEXT, b"answered\n")
    return {
        START_PATH: _start_page(page, same_site, cross_site),
        _LINK_SAME_ORIGIN.path: _link_page(same_site + _LINK_SAME_SITE.path),
        _LINK_SAME_SITE.path: _link_page(cross_site + _LINK_CROSS_SITE.path),
        _LINK_CROSS_SITE.path: _link_page(page + _FORMS_PATH),
        _FORMS_PATH: _page(
            f'<form method="post" action="{_FORM_URLENCODED.path}">{_URLENCODED_FIELDS}{_SUBMIT}</form>',
        ),
        _FORM_URLENCODED.path: _page(
            f'<form method="post" action="{_FORM_MULTIPART.path}" enctype="multipart/form-data">'
            f"{_MULTIPART_FIELDS}{_SUBMIT}</form>",
            script=_FILL_FILE_INPUT + _SHOW_NEXT_ON_LOAD,
        ),
        _FORM_MULTIPART.path: _link_page(_REDIRECT_NAVIGATION[0].path),
        _REDIRECT_NAVIGATION[0].path: _redirect(same_site + _REDIRECT_NAVIGATION[1].path),
        _REDIRECT_NAVIGATION[1].path: _redirect(cross_site + _REDIRECT_NAVIGATION[2].path),
        _REDIRECT_NAVIGATION[2].path: _redirect(page + _REDIRECT_NAVIGATION[3].path),
        _REDIRECT_NAVIGATION[3].path: _page(script=_report_done(_DONE_PATH)),
        _REDIRECT_FETCH[0].path: _redirect(same_site + _REDIRECT_FETCH[1].path),
        _REDIRECT_FETCH[1].path: _redirect(cross_site + _REDIRECT_FETCH[2].path),
        _REDIRECT_FETCH[2].path: _redirect(page + _REDIRECT_FETCH[3].path),
        _REDIRECT_FETCH[3].path: text,
        _FETCH_GET_SAME_SITE.path: text,
        _FETCH_GET_CROSS_SITE.path: text,
        _FETCH_POST_TEXT_SAME_ORIGIN.path: text,
        _FETCH_POST_TEXT_CROSS_SITE.path: text,
        _FETCH_POST_JSON_SAME_ORIGIN.path: text,
        _FETCH_POST_STREAM.path: text,
        _FETCH_POST_STREAM_REFUSED_PATH: text,
        _SCRIPT_SAME_SITE.path: script,
        _SCRIPT_CROSS_SITE.path: script,
        _IMAGE_SAME_SITE.path: image,
        _IMAGE_CROSS_SITE.path: image,
        # The cookie's path keeps it off every request but the one that shows it.
        _COOKIE_SET_PATH: _case_response(_TEXT, b"set\n", ("Set-Cookie", f"{_COOKIE}; Path={_COOKIE_CARRIED.path}")),
        _COOKIE_CARRIED.path: text,
        _REVALIDATION.path: _case_response(_TEXT, b"answered\n", ("ETag", _ETAG), cache_control="no-cache"),
        _PLAIN_HTTP_NAVIGATION.path: _plain_http_page(),
        _PLAIN_HTTP_FETCH.path: text,
        _PLAIN_HTTP_SCRIPT.path: script,
        _PLAIN_HTTP_IMAGE.path: image,
        _PROXY_NAVIGATION.path: _page(script=_report_done(_PROXY_DONE_PATH)),
        _DONE_PATH: done,
        _PLAIN_HTTP_DONE_PATH: done,
        _PROXY_DONE_PATH: done,
    }


def respond_on(scheme: str, port: int, request: RequestHead) -> Response:
    """Answer a request that came in on a port, as the case pages, the test page and their paths do."""
    target = request.target
    if target.startswith(_FAILED_PREFIX):
        # This is the only trace of what went wrong in a Chrome that nobody is looking at.
        _log.warning("the page failed: %s", unquote(target.removeprefix(_FAILED_PREFIX)))
    if target == _REVALIDATION.path and has_header(request, "if-none-match"):
        return Response(304, _TEXT, b"", "no-cache", (_EVERY_ORIGIN, ("ETag", _ETAG)))
    answer = _responses(scheme, port).get(target)
    return response_for(target) if answer is None else answer
