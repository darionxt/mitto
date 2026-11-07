"""The Recording Cases, the pages that make a real Chrome produce them, and the starts of Chrome that go through them."""

import dataclasses
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
FORMS_PATH = "/case/forms"
_DONE_PATH = "/case/done"
_PLAIN_HTTP_DONE_PATH = "/case/plain-http-done"
_PROXY_DONE_PATH = "/case/proxy-done"
_CLIENT_HINTS_DONE_PATH = "/case/client-hints-done"
_RESUMPTION_DONE_PATH = "/case/resumption-done"
RESUMPTION_CLOSE_PATH = "/case/resumption-close"
_RESUMPTION_FIRST_PATH = "/case/resumption-first"
RESUMPTION_OTHER_SITE_PATH = "/case/resumption-other-site"
_CLIENT_HINTS_PLAIN_HTTP_DONE_PATH = "/case/client-hints-plain-http-done"
CLIENT_HINTS_PLAIN_HTTP_START_PATH = "/case/client-hints-plain-http"
_FAILED_PREFIX = "/case/failed?"
COOKIE_SET_PATH = "/case/cookie-set"
_COOKIE = "mitto=1"

_CASE_PREFIX = "/case/"
_TEXT = "text/plain; charset=utf-8"
_HTML = "text/html; charset=utf-8"
_JAVASCRIPT = "text/javascript; charset=utf-8"
_PNG = "image/png"
_EVERY_ORIGIN = ("Access-Control-Allow-Origin", "*")
_ETAG = '"mitto"'

_ACCEPT_CH = "Accept-CH"
_CRITICAL_CH = "Critical-CH"
_EVERY_CLIENT_HINT = (
    "device-memory, dpr, width, viewport-width, rtt, downlink, ect, sec-ch-ua, sec-ch-ua-arch, sec-ch-ua-platform,"
    " sec-ch-ua-model, sec-ch-ua-mobile, sec-ch-ua-full-version, sec-ch-ua-platform-version,"
    " sec-ch-prefers-color-scheme, sec-ch-ua-bitness, sec-ch-viewport-height, sec-ch-device-memory, sec-ch-dpr,"
    " sec-ch-width, sec-ch-viewport-width, sec-ch-ua-full-version-list, sec-ch-ua-wow64, save-data,"
    " sec-ch-prefers-reduced-motion, sec-ch-ua-form-factors, sec-ch-prefers-reduced-transparency"
)
"""What a server asks for to get every client hint, as in GetClientHintToNameMap() in Chromium."""

_CRITICAL_HINT = "sec-ch-ua-arch"

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

_CH_FIRST = _case("client_hints_first")
_CH_SCRIPT = _case("client_hints_script")
_CH_SCRIPT_SAME_SITE = _case("client_hints_script_same_site")
_CH_SCRIPT_CROSS_SITE = _case("client_hints_script_cross_site")
_CH_IMAGE = _case("client_hints_image")
_CH_IMAGE_SAME_SITE = _case("client_hints_image_same_site")
_CH_IMAGE_CROSS_SITE = _case("client_hints_image_cross_site")
_CH_FETCH = _case("client_hints_fetch")
_CH_FETCH_SAME_SITE = _case("client_hints_fetch_same_site")
_CH_FETCH_CROSS_SITE = _case("client_hints_fetch_cross_site")
_CH_FETCH_POST = _case("client_hints_fetch_post")
_CH_REDIRECT_FETCH = tuple(_case(f"client_hints_redirect_fetch_{hop}") for hop in range(1, 5))
_CH_NAVIGATION = _case("client_hints_navigation")
_CH_NAVIGATION_SAME_SITE = _case("client_hints_navigation_same_site")
_CH_NAVIGATION_CROSS_SITE = _case("client_hints_navigation_cross_site")
_CH_NAVIGATION_BACK = _case("client_hints_navigation_back")
_CH_FORM = _case("client_hints_form")
_CH_REDIRECT_NAVIGATION = tuple(_case(f"client_hints_redirect_navigation_{hop}") for hop in range(1, 6))
_CH_CRITICAL = _case("client_hints_critical", carries=_CRITICAL_HINT)
_CH_PLAIN_HTTP_FETCH = _case("client_hints_plain_http_fetch")
_CH_PLAIN_HTTP_NAVIGATION = _case("client_hints_plain_http_navigation")
_RESUMPTION_FETCH = _case("resumption_fetch")
_RESUMPTION_POST = _case("resumption_post")
_RESUMPTION_NAVIGATION = _case("resumption_navigation")
_RESUMPTION_OTHER_SITE_SCRIPT = _case("resumption_other_site_script")


@dataclass(frozen=True, slots=True)
class HintsAsked:
    """A page that asks for its own set of hints, and the requests that show what that set does.

    Attributes:
        path: Where the page is, which is the page's origin. Its own request shows what the set before it did.
        accept_ch: The value of its Accept-CH header.
        fetch: A fetch() from the page.
        post: A fetch() with a text body, which makes one line more.
        navigation: A click on a link in the page.
    """

    path: str
    accept_ch: str
    fetch: Case
    post: Case
    navigation: Case


def _hints_asked(name: str, accept_ch: str) -> HintsAsked:
    return HintsAsked(
        f"{_CASE_PREFIX}client-hints-{name}",
        accept_ch,
        _case(f"client_hints_{name}_fetch"),
        _case(f"client_hints_{name}_post"),
        _case(f"client_hints_{name}_navigation"),
    )


# Chrome keeps a subresource's lines in a hash table, whose size follows how many there are. So these ask for as many as
# it takes to reach each size, with the sets real servers ask for among them.
HINTS_ASKED = (
    _hints_asked("one", "sec-ch-ua-arch"),
    _hints_asked("few", "sec-ch-ua-arch, sec-ch-ua-model, sec-ch-ua-platform-version"),
    _hints_asked(
        "many",
        "sec-ch-ua-bitness, sec-ch-ua-arch, sec-ch-ua-full-version, sec-ch-ua-mobile, sec-ch-ua-model,"
        " sec-ch-ua-platform-version, sec-ch-ua-full-version-list, sec-ch-ua-platform, sec-ch-ua",
    ),
    _hints_asked("legacy", "dpr, viewport-width, rtt, downlink, ect, device-memory"),
    # An empty header replaces what the origin asked for with nothing.
    _hints_asked("none", ""),
)

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
_CLIENT_HINTS_CASES = (
    _CH_FIRST,
    _CH_SCRIPT,
    _CH_SCRIPT_SAME_SITE,
    _CH_SCRIPT_CROSS_SITE,
    _CH_IMAGE,
    _CH_IMAGE_SAME_SITE,
    _CH_IMAGE_CROSS_SITE,
    _CH_FETCH,
    _CH_FETCH_SAME_SITE,
    _CH_FETCH_CROSS_SITE,
    _CH_FETCH_POST,
    *_CH_REDIRECT_FETCH,
    _CH_NAVIGATION,
    _CH_NAVIGATION_SAME_SITE,
    _CH_NAVIGATION_CROSS_SITE,
    _CH_NAVIGATION_BACK,
    _CH_FORM,
    *_CH_REDIRECT_NAVIGATION,
    *(case for asked in HINTS_ASKED for case in (asked.fetch, asked.post, asked.navigation)),
    _CH_CRITICAL,
)
_CLIENT_HINTS_PLAIN_HTTP_CASES = (_CH_PLAIN_HTTP_FETCH, _CH_PLAIN_HTTP_NAVIGATION)

CASES = (
    *MAIN_CASES,
    *_TLS_CASES,
    *_PLAIN_HTTP_CASES,
    *_PROXY_CASES,
    *_CLIENT_HINTS_CASES,
    *_CLIENT_HINTS_PLAIN_HTTP_CASES,
)
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
        fresh_cases: In a run that resumes, the Recording Cases whose connections have no ticket to use, where those of
            cases do. They go in a file of their own, named .fresh.json.
    """

    name: str
    label: str
    route: str
    host: str | None
    start_path: str
    done_path: str
    cases: tuple[Case, ...]
    fresh_cases: tuple[Case, ...] = ()


def _main_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, None, KIND_PATHS[RequestKind.NAVIGATION], DONE_PATH, MAIN_CASES)


def _tls_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, START_PATH, _DONE_PATH, _TLS_CASES)


def _proxy_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, _PROXY_NAVIGATION.path, _PROXY_DONE_PATH, _PROXY_CASES)


def _client_hints_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, _CH_FIRST.path, _CLIENT_HINTS_DONE_PATH, _CLIENT_HINTS_CASES)


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
    # Last, because a server's asking lasts as long as the user data directory does, and the origins are the ones above.
    _client_hints_run("cases/client_hints_http1", "Client hints over HTTP/1.1", HTTP1),
    _client_hints_run("cases/client_hints_http2", "Client hints over HTTP/2", HTTP2),
    _client_hints_run("cases/client_hints_http3", "Client hints over HTTP/3", HTTP3),
    Run(
        "cases/client_hints_plain_http",
        "Client hints over plain HTTP",
        PLAIN_HTTP,
        CASE_HOST,
        CLIENT_HINTS_PLAIN_HTTP_START_PATH,
        _CLIENT_HINTS_PLAIN_HTTP_DONE_PATH,
        _CLIENT_HINTS_PLAIN_HTTP_CASES,
    ),
)
"""Every start of a Chrome that a recording session takes, in the order it takes them."""


def _resumption_run(name: str, label: str, route: str) -> Run:
    return Run(
        name,
        label,
        route,
        CASE_HOST,
        _RESUMPTION_FIRST_PATH,
        _RESUMPTION_DONE_PATH,
        (_RESUMPTION_FETCH, _RESUMPTION_POST, _RESUMPTION_NAVIGATION),
        (_RESUMPTION_OTHER_SITE_SCRIPT,),
    )


RESUMPTION_RUNS = (
    _resumption_run("resumption/http1", "Resumed connections over HTTP/1.1", HTTP1),
    _resumption_run("resumption/http2", "Resumed connections over HTTP/2", HTTP2),
    _resumption_run("resumption/http3", "Resumed connections over HTTP/3", HTTP3),
)
"""The runs for a server that gives out tickets. Their connections would break a normal form of the runs above."""

DONE_PATHS = frozenset(run.done_path for run in (*RUNS, *RESUMPTION_RUNS))

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

# The images come from a script. One in the HTML goes through the preload scanner, which gives it another priority unless
# it says it is small. When it says so, its width goes in width and sec-ch-width, which only a page's layout can know.
_CLIENT_HINTS_FIRST_SCRIPT = """
  const imageLoaded = (url) => new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = resolve;
    image.onerror = () => reject(new Error(url + " failed to load"));
    image.src = url;
  });
  const images = [urls.image, urls.image_same_site, urls.image_cross_site].map(imageLoaded);
  addEventListener("load", () => {
    (async () => {
      await Promise.all(images);
      await get(urls.fetch);
      await get(urls.fetch_same_site);
      await get(urls.fetch_cross_site);
      await get(urls.fetch_post, {method: "POST", body: "text body"});
      await get(urls.redirect_fetch_1);
      showNext();
    })().catch(report);
  });
"""

_SHOW_NEXT_ON_LOAD = """
  addEventListener("load", showNext);
"""

_FILL_FILE_INPUT = """
  const files = new DataTransfer();
  files.items.add(new File(["file text\\n"], 'file "name".txt', {type: "text/plain"}));
  document.getElementById("file").files = files.files;
"""


# The wait gives the client time to see that the connection has ended, so that the next request does not try to use it.
_RESUMPTION_SCRIPT = """
  const closeConnection = () => get(urls.close).then(() => new Promise((resolve) => setTimeout(resolve, 500)));
  addEventListener("load", () => {
    (async () => {
      await closeConnection();
      await get(urls.fetch);
      await closeConnection();
      await get(urls.post, {method: "POST", body: "text body"});
      await closeConnection();
      location.href = urls.navigation;
    })().catch(report);
  });
"""

# A script goes with credentials, as a navigation does, so what keeps its ticket from the navigation is the top-level site.
_RESUMPTION_NAVIGATION_SCRIPT = """
  const wait = () => new Promise((resolve) => setTimeout(resolve, 500));
  const scriptLoaded = (url) => new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = url;
    script.onload = resolve;
    script.onerror = () => reject(new Error(url + " failed to load"));
    document.head.append(script);
  });
  addEventListener("load", () => {
    (async () => {
      await get(urls.close);
      await wait();
      await scriptLoaded(urls.script);
      await wait();
      location.href = urls.otherSite;
    })().catch(report);
  });
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


def _page(*parts: str, script: str = _SHOW_NEXT_ON_LOAD, headers: tuple[tuple[str, str], ...] = ()) -> Response:
    """Return a page that shows a progress line, holds parts, and runs script once it has what parts define."""
    body = "\n".join(('<p id="progress">Recording...</p>', *parts, f"<script>{_PAGE_SCRIPT}{script}</script>"))
    return _case_response(_HTML, _document(body), *headers)


def _case_response(
    content_type: str, body: bytes, *headers: tuple[str, str], cache_control: str = "no-store"
) -> Response:
    # Everything answers every origin, because what a page asks of another origin only works if the answer allows it,
    # and a request carries no sign of that.
    return Response(200, content_type, body, cache_control, (_EVERY_ORIGIN, *headers))


def _link(to: str) -> str:
    return f'<p><a id="next" hidden href="{to}">Click this link.</a></p>'


def _link_page(to: str, *headers: tuple[str, str]) -> Response:
    return _page(_link(to), headers=headers)


def _redirect(to: str, *headers: tuple[str, str]) -> Response:
    return Response(302, _TEXT, b"", headers=(_EVERY_ORIGIN, ("Location", to), *headers))


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
        "cookie_set": COOKIE_SET_PATH,
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


def _client_hints_first_page(page: str, same_site: str, cross_site: str) -> Response:
    urls = {
        "image": _CH_IMAGE.path,
        "image_same_site": same_site + _CH_IMAGE_SAME_SITE.path,
        "image_cross_site": cross_site + _CH_IMAGE_CROSS_SITE.path,
        "fetch": _CH_FETCH.path,
        "fetch_same_site": same_site + _CH_FETCH_SAME_SITE.path,
        "fetch_cross_site": cross_site + _CH_FETCH_CROSS_SITE.path,
        "fetch_post": _CH_FETCH_POST.path,
        "redirect_fetch_1": _CH_REDIRECT_FETCH[0].path,
    }
    return _page(
        f'<script src="{_CH_SCRIPT.path}"></script>',
        f'<script src="{same_site}{_CH_SCRIPT_SAME_SITE.path}"></script>',
        f'<script src="{cross_site}{_CH_SCRIPT_CROSS_SITE.path}"></script>',
        _link(page + _CH_NAVIGATION.path),
        f"<script>const urls = {json.dumps(urls)};</script>",
        script=_CLIENT_HINTS_FIRST_SCRIPT,
        headers=((_ACCEPT_CH, _EVERY_CLIENT_HINT),),
    )


def _hints_asked_page(asked: HintsAsked) -> Response:
    script = f"""
  get("{asked.fetch.path}")
    .then(() => get("{asked.post.path}", {{method: "POST", body: "text body"}}))
    .then(showNext)
    .catch(report);
"""
    return _page(_link(asked.navigation.path), script=script, headers=((_ACCEPT_CH, asked.accept_ch),))


def _hints_asked_responses(cross_site: str, text: Response) -> dict[str, Response]:
    """Return the answers to the pages of HINTS_ASKED. They go through each in turn, and on to the last case."""
    responses: dict[str, Response] = {}
    next_paths = (*(asked.path for asked in HINTS_ASKED[1:]), cross_site + _CH_CRITICAL.path)
    for asked, next_path in zip(HINTS_ASKED, next_paths, strict=True):
        responses[asked.path] = _hints_asked_page(asked)
        responses[asked.fetch.path] = text
        responses[asked.post.path] = text
        responses[asked.navigation.path] = _link_page(next_path)
    return responses


def _client_hints_plain_http_page() -> Response:
    script = f"""
  addEventListener("load", () => {{
    get("{_CH_PLAIN_HTTP_FETCH.path}")
      .then(() => {{ location.href = "{_CH_PLAIN_HTTP_NAVIGATION.path}"; }})
      .catch(report);
  }});
"""
    return _page(script=script, headers=((_ACCEPT_CH, _EVERY_CLIENT_HINT),))


def _client_hints_responses(
    page: str, same_site: str, cross_site: str, done: Response, script: Response, image: Response, text: Response
) -> dict[str, Response]:
    """Return the answers of the client hints runs, which go through these pages in this order.

    The page's origin asks for every hint. What another origin's subresources say has no effect, because only a
    navigation's response counts. The other origin of the site then asks on a redirect, which counts at once. The other
    site stays out of it until the end, when it asks in a way that makes Chrome send the request again.
    """
    asks = (_ACCEPT_CH, _EVERY_CLIENT_HINT)
    ignored_text = _case_response(_TEXT, b"answered\n", asks)
    ignored_script = _case_response(_JAVASCRIPT, b"// Nothing to do.\n", asks)
    ignored_image = _case_response(_PNG, image.body, asks)
    return {
        _CH_FIRST.path: _client_hints_first_page(page, same_site, cross_site),
        _CH_SCRIPT.path: script,
        _CH_SCRIPT_SAME_SITE.path: ignored_script,
        _CH_SCRIPT_CROSS_SITE.path: ignored_script,
        _CH_IMAGE.path: image,
        _CH_IMAGE_SAME_SITE.path: ignored_image,
        _CH_IMAGE_CROSS_SITE.path: ignored_image,
        _CH_FETCH.path: text,
        _CH_FETCH_SAME_SITE.path: ignored_text,
        _CH_FETCH_CROSS_SITE.path: ignored_text,
        _CH_FETCH_POST.path: text,
        _CH_REDIRECT_FETCH[0].path: _redirect(same_site + _CH_REDIRECT_FETCH[1].path),
        _CH_REDIRECT_FETCH[1].path: _redirect(cross_site + _CH_REDIRECT_FETCH[2].path),
        _CH_REDIRECT_FETCH[2].path: _redirect(page + _CH_REDIRECT_FETCH[3].path),
        _CH_REDIRECT_FETCH[3].path: text,
        _CH_NAVIGATION.path: _link_page(same_site + _CH_NAVIGATION_SAME_SITE.path),
        _CH_NAVIGATION_SAME_SITE.path: _link_page(cross_site + _CH_NAVIGATION_CROSS_SITE.path),
        _CH_NAVIGATION_CROSS_SITE.path: _link_page(page + _CH_NAVIGATION_BACK.path),
        _CH_NAVIGATION_BACK.path: _page(
            f'<form method="post" action="{_CH_FORM.path}">{_URLENCODED_FIELDS}{_SUBMIT}</form>',
        ),
        _CH_FORM.path: _link_page(_CH_REDIRECT_NAVIGATION[0].path),
        _CH_REDIRECT_NAVIGATION[0].path: _redirect(same_site + _CH_REDIRECT_NAVIGATION[1].path),
        _CH_REDIRECT_NAVIGATION[1].path: _redirect(same_site + _CH_REDIRECT_NAVIGATION[2].path, asks),
        _CH_REDIRECT_NAVIGATION[2].path: _redirect(cross_site + _CH_REDIRECT_NAVIGATION[3].path),
        _CH_REDIRECT_NAVIGATION[3].path: _redirect(page + _CH_REDIRECT_NAVIGATION[4].path),
        _CH_REDIRECT_NAVIGATION[4].path: _link_page(HINTS_ASKED[0].path),
        **_hints_asked_responses(cross_site, text),
        _CH_CRITICAL.path: _page(
            script=_report_done(_CLIENT_HINTS_DONE_PATH),
            headers=((_ACCEPT_CH, _CRITICAL_HINT), (_CRITICAL_CH, _CRITICAL_HINT)),
        ),
        _CLIENT_HINTS_DONE_PATH: done,
        CLIENT_HINTS_PLAIN_HTTP_START_PATH: _client_hints_plain_http_page(),
        _CH_PLAIN_HTTP_FETCH.path: text,
        _CH_PLAIN_HTTP_NAVIGATION.path: _page(script=_report_done(_CLIENT_HINTS_PLAIN_HTTP_DONE_PATH)),
        _CLIENT_HINTS_PLAIN_HTTP_DONE_PATH: done,
    }


def _resumption_responses(cross_site: str, done: Response, script: Response, text: Response) -> dict[str, Response]:
    """Return the answers of the resumption runs, which end a connection before each thing they want to see.

    Each of the requests after the first therefore opens a new connection. What it sends first depends on the request,
    because a client only sends what is safe to repeat as early data, so each kind has a connection of its own. Then
    a script at another site brings a ticket for that site, which the navigation there cannot use if tickets go by
    top-level site.
    """
    first_urls = {
        "close": RESUMPTION_CLOSE_PATH,
        "fetch": _RESUMPTION_FETCH.path,
        "post": _RESUMPTION_POST.path,
        "navigation": _RESUMPTION_NAVIGATION.path,
    }
    navigation_urls = {
        "close": RESUMPTION_CLOSE_PATH,
        "script": cross_site + _RESUMPTION_OTHER_SITE_SCRIPT.path,
        "otherSite": cross_site + RESUMPTION_OTHER_SITE_PATH,
    }
    return {
        _RESUMPTION_FIRST_PATH: _page(
            f"<script>const urls = {json.dumps(first_urls)};</script>", script=_RESUMPTION_SCRIPT
        ),
        RESUMPTION_CLOSE_PATH: dataclasses.replace(text, close=True),
        _RESUMPTION_FETCH.path: text,
        _RESUMPTION_POST.path: text,
        _RESUMPTION_NAVIGATION.path: _page(
            f"<script>const urls = {json.dumps(navigation_urls)};</script>", script=_RESUMPTION_NAVIGATION_SCRIPT
        ),
        _RESUMPTION_OTHER_SITE_SCRIPT.path: dataclasses.replace(script, close=True),
        RESUMPTION_OTHER_SITE_PATH: _page(script=_report_done(_RESUMPTION_DONE_PATH)),
        _RESUMPTION_DONE_PATH: done,
    }


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
        _LINK_CROSS_SITE.path: _link_page(page + FORMS_PATH),
        FORMS_PATH: _page(
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
        COOKIE_SET_PATH: _case_response(_TEXT, b"set\n", ("Set-Cookie", f"{_COOKIE}; Path={_COOKIE_CARRIED.path}")),
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
        **_client_hints_responses(page, same_site, cross_site, done, script, image, text),
        **_resumption_responses(cross_site, done, script, text),
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
