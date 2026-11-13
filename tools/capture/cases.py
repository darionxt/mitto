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
_DOCUMENT_START_PATH = "/case/documents-start"
_DOCUMENT_DONE_PATH = "/case/documents-done"
_DOCUMENT_FRAMES_PATH = "/case/documents-frames"
_DOCUMENT_PATH = "/case/document"
_DOCUMENT_FORMS_PATH = "/case/documents-forms"
_CREDENTIALS_COOKIE_SET_PATH = "/case/credentials-cookie-set"
_HISTORY_NEXT_PATH = "/case/history-next"
_HISTORY_COOKIE_SET_PATH = "/case/history-cookie-set"
_HISTORY_COOKIE_CLEAR_PATH = "/case/history-cookie-clear"

_CASE_PREFIX = "/case/"
_TEXT = "text/plain; charset=utf-8"
_HTML = "text/html; charset=utf-8"
_JAVASCRIPT = "text/javascript; charset=utf-8"
_PNG = "image/png"
_CSS = "text/css; charset=utf-8"
_WOFF2 = "font/woff2"
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

# What a document requests beside the main Request Kinds, and the navigations it starts.
_STYLE = _case("style")
_STYLE_IMPORT = _case("style_import")
_STYLE_CROSS_SITE = _case("style_cross_site")
_FONT = _case("font")
_FONT_CROSS_SITE = _case("font_cross_site")
_MODULE = _case("module")
_MODULE_STATIC_IMPORT = _case("module_static_import")
_MODULE_DYNAMIC_IMPORT = _case("module_dynamic_import")
_MODULE_CROSS_SITE = _case("module_cross_site")
_XHR_GET = _case("xhr_get")
_XHR_GET_CROSS_SITE = _case("xhr_get_cross_site")
_XHR_POST_TEXT = _case("xhr_post_text")
_XHR_GET_SYNC = _case("xhr_get_sync")
_XHR_POST_TEXT_SYNC = _case("xhr_post_text_sync")
_WORKER = _case("worker")
_WORKER_IMPORT_SCRIPTS = _case("worker_import_scripts")
_WORKER_FETCH = _case("worker_fetch")
_WORKER_MODULE = _case("worker_module")
_BEACON_TEXT = _case("beacon_text")
_BEACON_BLOB = _case("beacon_blob")
_BEACON_FORM_DATA = _case("beacon_form_data")
_BEACON_TEXT_CROSS_SITE = _case("beacon_text_cross_site")
_FETCH_LATER = _case("fetch_later")
# A preflight goes to the path of the request it asks for, so each of the two carries what the other lacks.
_FETCH_POST_JSON_CROSS_SITE = _case("fetch_post_json_cross_site", carries="content-type")
_PREFLIGHT = Case("preflight", _FETCH_POST_JSON_CROSS_SITE.path, "access-control-request-method")
_FETCH_CREDENTIALS_OMIT = _case("fetch_credentials_omit")
_FETCH_CREDENTIALS_SAME_ORIGIN = _case("fetch_credentials_same_origin")
_FETCH_CREDENTIALS_INCLUDE = _case("fetch_credentials_include")
_FETCH_NO_CORS = _case("fetch_no_cors")
_NAVIGATION_LOCATION = _case("navigation_location")
_NAVIGATION_CLICK = _case("navigation_click")
_NAVIGATION_META_REFRESH = _case("navigation_meta_refresh")
_REDIRECT_LOCATION = tuple(_case(f"redirect_location_{hop}") for hop in range(1, 3))
_FRAME_SAME_ORIGIN = _case("frame_same_origin")
_FRAME_SAME_SITE = _case("frame_same_site")
_FRAME_CROSS_SITE = _case("frame_cross_site")
_FRAME_SCRIPT = _case("frame_script")
_FRAME_IMAGE = _case("frame_image")
_FRAME_FETCH = _case("frame_fetch")
_DOCUMENT_STYLE = _case("document_style")
_DOCUMENT_PRELOAD_STYLE = _case("document_preload_style")
_DOCUMENT_PRELOAD_SCRIPT = _case("document_preload_script")
_DOCUMENT_PRELOAD_IMAGE = _case("document_preload_image")
_DOCUMENT_PRELOAD_FONT = _case("document_preload_font")
_DOCUMENT_SCRIPT = _case("document_script")
_DOCUMENT_IMAGE_SMALL = _case("document_image_small")
_DOCUMENT_IMAGE = _case("document_image")
_DOCUMENT_STYLE_BODY = _case("document_style_body")
_DOCUMENT_SCRIPT_DEFERRED = _case("document_script_deferred")
_FAVICON_NOT_ASKED_PATH = "/case/favicon-not-asked"
# Chrome asks for it by itself, at a path of its own, if it asks at all.
_FAVICON = Case("favicon", "/favicon.ico", refused_path=_FAVICON_NOT_ASKED_PATH)
_URLENCODED_QUERY = "name=mitto&a+b%26c=d+e%26f%2Bg"
"""What the fields of the forms make, urlencoded."""
# A form that GETs puts its fields in the query, which is part of what stands for the case.
_FORM_GET = Case("navigation_form_get", f"{_CASE_PREFIX}navigation-form-get?{_URLENCODED_QUERY}")
_FORM_TEXT = _case("navigation_form_text")
_FORM_CROSS_SITE = _case("navigation_form_cross_site")
# The first visit to its page has no cookie, and the traversal back does.
_HISTORY_BACK = _case("navigation_history_back", carries="cookie")


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
_DOCUMENT_ORDERED = (
    _DOCUMENT_STYLE,
    _DOCUMENT_PRELOAD_STYLE,
    _DOCUMENT_PRELOAD_SCRIPT,
    _DOCUMENT_PRELOAD_IMAGE,
    _DOCUMENT_PRELOAD_FONT,
    _DOCUMENT_SCRIPT,
    _DOCUMENT_IMAGE_SMALL,
    _DOCUMENT_IMAGE,
    _DOCUMENT_STYLE_BODY,
    _DOCUMENT_SCRIPT_DEFERRED,
)
"""What the document's markup asks for, whose order on the wire is Chrome's to choose."""

DOCUMENT_CASES = (
    _STYLE,
    _STYLE_IMPORT,
    _STYLE_CROSS_SITE,
    _FONT,
    _FONT_CROSS_SITE,
    _MODULE,
    _MODULE_STATIC_IMPORT,
    _MODULE_DYNAMIC_IMPORT,
    _MODULE_CROSS_SITE,
    _XHR_GET,
    _XHR_GET_CROSS_SITE,
    _XHR_POST_TEXT,
    _XHR_GET_SYNC,
    _XHR_POST_TEXT_SYNC,
    _WORKER,
    _WORKER_IMPORT_SCRIPTS,
    _WORKER_FETCH,
    _WORKER_MODULE,
    _BEACON_TEXT,
    _BEACON_BLOB,
    _BEACON_FORM_DATA,
    _BEACON_TEXT_CROSS_SITE,
    _FETCH_LATER,
    _PREFLIGHT,
    _FETCH_POST_JSON_CROSS_SITE,
    _FETCH_CREDENTIALS_OMIT,
    _FETCH_CREDENTIALS_SAME_ORIGIN,
    _FETCH_CREDENTIALS_INCLUDE,
    _FETCH_NO_CORS,
    _NAVIGATION_LOCATION,
    _NAVIGATION_CLICK,
    _NAVIGATION_META_REFRESH,
    *_REDIRECT_LOCATION,
    _FRAME_SAME_ORIGIN,
    _FRAME_SAME_SITE,
    _FRAME_CROSS_SITE,
    _FRAME_SCRIPT,
    _FRAME_IMAGE,
    _FRAME_FETCH,
    *_DOCUMENT_ORDERED,
    _FAVICON,
    _FORM_GET,
    _FORM_TEXT,
    _FORM_CROSS_SITE,
    _HISTORY_BACK,
)
"""The Recording Cases of what a document requests beside the main Request Kinds, in the order a normal form lists them.

The Parity Checks of the wire do not replay these yet, so they are not among CASES."""

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
        ordered: Recording Cases among cases whose order on the wire its normal form writes down.
    """

    name: str
    label: str
    route: str
    host: str | None
    start_path: str
    done_path: str
    cases: tuple[Case, ...]
    fresh_cases: tuple[Case, ...] = ()
    ordered: tuple[Case, ...] = ()


def _main_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, None, KIND_PATHS[RequestKind.NAVIGATION], DONE_PATH, MAIN_CASES)


def _tls_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, START_PATH, _DONE_PATH, _TLS_CASES)


def _proxy_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, _PROXY_NAVIGATION.path, _PROXY_DONE_PATH, _PROXY_CASES)


def _client_hints_run(name: str, label: str, route: str) -> Run:
    return Run(name, label, route, CASE_HOST, _CH_FIRST.path, _CLIENT_HINTS_DONE_PATH, _CLIENT_HINTS_CASES)


def _document_run(name: str, label: str, route: str) -> Run:
    return Run(
        name,
        label,
        route,
        CASE_HOST,
        _DOCUMENT_START_PATH,
        _DOCUMENT_DONE_PATH,
        DOCUMENT_CASES,
        ordered=_DOCUMENT_ORDERED,
    )


_FIRST_RUNS = (
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
# Last, because a server's asking lasts as long as the user data directory does, and the origins are the ones above.
_CLIENT_HINTS_RUNS = (
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

RUNS = (*_FIRST_RUNS, *_CLIENT_HINTS_RUNS)
"""The runs of the Recording Cases that the Parity Checks of the wire replay, in the order a recording session takes
them."""

DOCUMENT_RUNS = (
    _document_run("documents/http1", "Documents over HTTP/1.1", HTTP1),
    _document_run("documents/http2", "Documents over HTTP/2", HTTP2),
    _document_run("documents/http3", "Documents over HTTP/3", HTTP3),
)
"""The runs of DOCUMENT_CASES. Their files go to a directory of their own, where the Parity Checks of the wire do not
look, until those replay them."""

RECORDING_RUNS = (*_FIRST_RUNS, *DOCUMENT_RUNS, *_CLIENT_HINTS_RUNS)
"""Every start of a Chrome that a recording session takes for the wire, in the order it takes them."""


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

DONE_PATHS = frozenset(run.done_path for run in (*RECORDING_RUNS, *RESUMPTION_RUNS))

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


# The page lays its text out at once, which is when Chrome starts on the fonts that the text uses, and then waits for
# them. Their answer is no font, so they fail, after their requests.
_DOCUMENTS_START_SCRIPT = """
  const moduleRan = new Promise((resolve) => addEventListener("mitto-module", resolve, {once: true}));
  const fontsLoaded = () => {
    document.body.getBoundingClientRect();
    return Promise.all([...document.fonts].map((face) => face.status === "unloaded"
      ? Promise.reject(new Error(face.family + " was not used"))
      : face.loaded.catch(() => undefined)));
  };
  const xhr = (method, url, body, async) => new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open(method, url, async);
    request.onload = resolve;
    request.onerror = () => reject(new Error(url + " failed"));
    request.send(body);
    if (!async) resolve();
  });
  const workerAnswered = (url, options) => new Promise((resolve, reject) => {
    const worker = new Worker(url, options);
    worker.onmessage = () => {
      worker.terminate();
      resolve();
    };
    worker.onerror = () => reject(new Error(url + " failed"));
  });
  addEventListener("load", () => {
    (async () => {
      await fontsLoaded();
      await moduleRan;
      await xhr("GET", urls.xhr_get, null, true);
      await xhr("GET", urls.xhr_get_cross_site, null, true);
      await xhr("POST", urls.xhr_post_text, "text body", true);
      await xhr("GET", urls.xhr_get_sync, null, false);
      await xhr("POST", urls.xhr_post_text_sync, "text body", false);
      await workerAnswered(urls.worker);
      await workerAnswered(urls.worker_module, {type: "module"});
      const fields = new FormData();
      fields.append("name", "mitto");
      const queued = [
        navigator.sendBeacon(urls.beacon_text, "text body"),
        navigator.sendBeacon(urls.beacon_blob, new Blob(["blob body"])),
        navigator.sendBeacon(urls.beacon_form_data, fields),
        navigator.sendBeacon(urls.beacon_text_cross_site, "text body"),
      ];
      if (queued.includes(false)) throw new Error("Chrome did not queue every beacon");
      // It goes when this document does.
      fetchLater(urls.fetch_later);
      await get(urls.fetch_post_json_cross_site, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: '{"json":"body"}',
      });
      await get(urls.credentials_cookie_set, {credentials: "include"});
      await get(urls.fetch_credentials_omit, {credentials: "omit"});
      await get(urls.fetch_credentials_same_origin, {credentials: "same-origin"});
      await get(urls.fetch_credentials_include, {credentials: "include"});
      await get(urls.fetch_no_cors, {mode: "no-cors"});
      location.href = urls.navigation_location;
    })().catch(report);
  });
"""

# A module that imports two more, and then says so to the page. The page waits for its event before it goes on.
_MODULE_SOURCE = f"""import "{_MODULE_STATIC_IMPORT.path}";
await import("{_MODULE_DYNAMIC_IMPORT.path}");
dispatchEvent(new Event("mitto-module"));
"""

_WORKER_SOURCE = f"""importScripts("{_WORKER_IMPORT_SCRIPTS.path}");
fetch("{_WORKER_FETCH.path}").then((response) => response.text()).then(() => postMessage("done"));
"""

_WORKER_MODULE_SOURCE = """postMessage("done");
"""

# The frame of another site asks for what a frame's own document asks for, and then tells the page.
_CROSS_SITE_FRAME = f"""<script src="{_FRAME_SCRIPT.path}"></script>
<img src="{_FRAME_IMAGE.path}" width="1" height="1" alt="">
<script>
  fetch("{_FRAME_FETCH.path}")
    .then((response) => response.text())
    .then(() => parent.postMessage("framed", "*"))
    .catch((error) => fetch("{_FAILED_PREFIX}" + encodeURIComponent(error)));
</script>
"""

# Chrome asks for /favicon.ico after a load, if it asks at all, so the page gives it time before it says that it did
# not, which counts only if Chrome did not.
_DOCUMENT_SCRIPT_SOURCE = f"""
  addEventListener("load", () => {{
    setTimeout(() => get("{_FAVICON_NOT_ASKED_PATH}").then(showNext).catch(report), 3000);
  }});
"""

# A no-store page that a script's request got a no-store answer for stays out of the back/forward cache, as does one
# whose cookies changed. Either way the traversal back to it is a request. The cookie, which only that request
# carries, goes again, because cookies do not go by port and the next run must start without it.
_HISTORY_BACK_SCRIPT = f"""
  addEventListener("pageshow", (event) => {{
    if (event.persisted) {{
      report(new Error("the page came back from the back/forward cache"));
      get("{_DOCUMENT_DONE_PATH}");
    }}
  }});
  addEventListener("load", () => {{
    const [entry] = performance.getEntriesByType("navigation");
    const traversed = entry.type === "back_forward";
    get("{_HISTORY_COOKIE_CLEAR_PATH}")
      .then(() => traversed ? get("{_DOCUMENT_DONE_PATH}") : showNext())
      .then(() => {{ if (traversed) progress.textContent = "Recorded. You can close this window."; }})
      .catch(report);
  }});
"""

_HISTORY_NEXT_SCRIPT = f"""
  addEventListener("load", () => {{
    get("{_HISTORY_COOKIE_SET_PATH}").then(() => history.back()).catch(report);
  }});
"""


def _navigate_on_load(to: str) -> str:
    return f"""
  addEventListener("load", () => {{ location.href = "{to}"; }});
"""


def _documents_start_page(same_site: str, cross_site: str) -> Response:
    urls = {
        "xhr_get": _XHR_GET.path,
        "xhr_get_cross_site": cross_site + _XHR_GET_CROSS_SITE.path,
        "xhr_post_text": _XHR_POST_TEXT.path,
        "xhr_get_sync": _XHR_GET_SYNC.path,
        "xhr_post_text_sync": _XHR_POST_TEXT_SYNC.path,
        "worker": _WORKER.path,
        "worker_module": _WORKER_MODULE.path,
        "beacon_text": _BEACON_TEXT.path,
        "beacon_blob": _BEACON_BLOB.path,
        "beacon_form_data": _BEACON_FORM_DATA.path,
        "beacon_text_cross_site": cross_site + _BEACON_TEXT_CROSS_SITE.path,
        "fetch_later": _FETCH_LATER.path,
        "fetch_post_json_cross_site": cross_site + _FETCH_POST_JSON_CROSS_SITE.path,
        "credentials_cookie_set": same_site + _CREDENTIALS_COOKIE_SET_PATH,
        "fetch_credentials_omit": same_site + _FETCH_CREDENTIALS_OMIT.path,
        "fetch_credentials_same_origin": same_site + _FETCH_CREDENTIALS_SAME_ORIGIN.path,
        "fetch_credentials_include": same_site + _FETCH_CREDENTIALS_INCLUDE.path,
        "fetch_no_cors": cross_site + _FETCH_NO_CORS.path,
        "navigation_location": _NAVIGATION_LOCATION.path,
    }
    return _page(
        f'<link rel="stylesheet" href="{_STYLE.path}">',
        f'<link rel="stylesheet" href="{cross_site}{_STYLE_CROSS_SITE.path}">',
        '<p class="mitto">Text in a web font.</p>',
        '<p class="mitto-cross-site">Text in a web font of another site.</p>',
        f'<script type="module" src="{_MODULE.path}"></script>',
        f'<script type="module" src="{cross_site}{_MODULE_CROSS_SITE.path}"></script>',
        f"<script>const urls = {json.dumps(urls)};</script>",
        script=_DOCUMENTS_START_SCRIPT,
    )


def _style_source(cross_site: str) -> bytes:
    return f"""@import url("{_STYLE_IMPORT.path}");
@font-face {{ font-family: mitto; src: url("{_FONT.path}"); }}
@font-face {{ font-family: mitto-cross-site; src: url("{cross_site}{_FONT_CROSS_SITE.path}"); }}
.mitto {{ font-family: mitto; }}
.mitto-cross-site {{ font-family: mitto-cross-site; }}
""".encode()


def _document_page() -> Response:
    """Return a document whose markup asks for what most pages' does, and which names no icon."""
    body = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>mitto capture</title>
<link rel="stylesheet" href="{_DOCUMENT_STYLE.path}">
<link rel="preload" href="{_DOCUMENT_PRELOAD_STYLE.path}" as="style">
<link rel="preload" href="{_DOCUMENT_PRELOAD_SCRIPT.path}" as="script">
<link rel="preload" href="{_DOCUMENT_PRELOAD_IMAGE.path}" as="image">
<link rel="preload" href="{_DOCUMENT_PRELOAD_FONT.path}" as="font" type="font/woff2" crossorigin>
<script src="{_DOCUMENT_SCRIPT.path}"></script>
</head>
<body>
<p id="progress">Recording...</p>
<img src="{_DOCUMENT_IMAGE_SMALL.path}" width="1" height="1" alt="">
<img src="{_DOCUMENT_IMAGE.path}" alt="">
<link rel="stylesheet" href="{_DOCUMENT_STYLE_BODY.path}">
<script src="{_DOCUMENT_SCRIPT_DEFERRED.path}" defer></script>
{_link(_DOCUMENT_FORMS_PATH)}
<script>{_PAGE_SCRIPT}{_DOCUMENT_SCRIPT_SOURCE}</script>
</body>
</html>
""".encode()
    return _case_response(_HTML, body)


def _form(action: str, *, method: str = "post", enctype: str | None = None) -> str:
    encoding = "" if enctype is None else f' enctype="{enctype}"'
    return f'<form method="{method}" action="{action}"{encoding}>{_URLENCODED_FIELDS}{_SUBMIT}</form>'


def _with_credentials(page: str, *headers: tuple[str, str]) -> Response:
    """Return an answer that a request with credentials from the page can read, which one that allows every origin is
    not."""
    allowed = (("Access-Control-Allow-Origin", page), ("Access-Control-Allow-Credentials", "true"))
    return Response(200, _TEXT, b"answered\n", headers=(*allowed, *headers))


def _document_responses(
    page: str, same_site: str, cross_site: str, done: Response, script: Response, image: Response, text: Response
) -> dict[str, Response]:
    """Return the answers of the runs of DOCUMENT_CASES, which go through these pages in this order.

    The first page's markup and script ask for each subresource and make each request of a script that the cases
    have, and the page then starts a navigation, whose page starts the next, and so on. The page after those has frames,
    and the next is a document whose markup asks for what most pages' does. Forms follow, and last comes a traversal
    back through history.
    """
    style = _case_response(_CSS, b"/* Nothing to do. */\n")
    # Chrome asks for a font before it can tell that the answer is none.
    font = _case_response(_WOFF2, b"no font\n")
    frame = _case_response(_HTML, _document("<p>A frame.</p>"))
    cookies = tuple(
        ("Set-Cookie", f"{_COOKIE}; Path={case.path}")
        for case in (_FETCH_CREDENTIALS_OMIT, _FETCH_CREDENTIALS_SAME_ORIGIN, _FETCH_CREDENTIALS_INCLUDE)
    )
    return {
        _DOCUMENT_START_PATH: _documents_start_page(same_site, cross_site),
        _STYLE.path: _case_response(_CSS, _style_source(cross_site)),
        _STYLE_IMPORT.path: style,
        _STYLE_CROSS_SITE.path: style,
        _FONT.path: font,
        _FONT_CROSS_SITE.path: font,
        _MODULE.path: _case_response(_JAVASCRIPT, _MODULE_SOURCE.encode()),
        _MODULE_STATIC_IMPORT.path: script,
        _MODULE_DYNAMIC_IMPORT.path: script,
        _MODULE_CROSS_SITE.path: script,
        _XHR_GET.path: text,
        _XHR_GET_CROSS_SITE.path: text,
        _XHR_POST_TEXT.path: text,
        _XHR_GET_SYNC.path: text,
        _XHR_POST_TEXT_SYNC.path: text,
        _WORKER.path: _case_response(_JAVASCRIPT, _WORKER_SOURCE.encode()),
        _WORKER_IMPORT_SCRIPTS.path: script,
        _WORKER_FETCH.path: text,
        _WORKER_MODULE.path: _case_response(_JAVASCRIPT, _WORKER_MODULE_SOURCE.encode()),
        _BEACON_TEXT.path: text,
        _BEACON_BLOB.path: text,
        _BEACON_FORM_DATA.path: text,
        _BEACON_TEXT_CROSS_SITE.path: text,
        _FETCH_LATER.path: text,
        # The answer to the preflight and to the request after it.
        _FETCH_POST_JSON_CROSS_SITE.path: _case_response(
            _TEXT, b"answered\n", ("Access-Control-Allow-Headers", "content-type")
        ),
        # The cookies go by path, so that each request but those of these cases goes without them.
        _CREDENTIALS_COOKIE_SET_PATH: _with_credentials(page, *cookies),
        _FETCH_CREDENTIALS_OMIT.path: _with_credentials(page),
        _FETCH_CREDENTIALS_SAME_ORIGIN.path: _with_credentials(page),
        _FETCH_CREDENTIALS_INCLUDE.path: _with_credentials(page),
        _FETCH_NO_CORS.path: text,
        _NAVIGATION_LOCATION.path: _page(
            f'<p><a id="link" href="{_NAVIGATION_CLICK.path}">The page clicks this link.</a></p>',
            script='\n  addEventListener("load", () => document.getElementById("link").click());\n',
        ),
        _NAVIGATION_CLICK.path: _case_response(
            _HTML,
            _document(
                f'<meta http-equiv="refresh" content="0; url={_NAVIGATION_META_REFRESH.path}">\n<p>Recording...</p>'
            ),
        ),
        _NAVIGATION_META_REFRESH.path: _page(script=_navigate_on_load(_REDIRECT_LOCATION[0].path)),
        _REDIRECT_LOCATION[0].path: _redirect(cross_site + _REDIRECT_LOCATION[1].path),
        _REDIRECT_LOCATION[1].path: _page(script=_navigate_on_load(page + _DOCUMENT_FRAMES_PATH)),
        _DOCUMENT_FRAMES_PATH: _page(
            '<script>const framed = new Promise((resolve) => addEventListener("message", resolve, {once: true}));'
            "</script>",
            f'<iframe src="{_FRAME_SAME_ORIGIN.path}"></iframe>',
            f'<iframe src="{same_site}{_FRAME_SAME_SITE.path}"></iframe>',
            f'<iframe src="{cross_site}{_FRAME_CROSS_SITE.path}"></iframe>',
            _link(_DOCUMENT_PATH),
            script='\n  addEventListener("load", () => { framed.then(showNext).catch(report); });\n',
        ),
        _FRAME_SAME_ORIGIN.path: frame,
        _FRAME_SAME_SITE.path: frame,
        _FRAME_CROSS_SITE.path: _case_response(_HTML, _document(_CROSS_SITE_FRAME)),
        _FRAME_SCRIPT.path: script,
        _FRAME_IMAGE.path: image,
        _FRAME_FETCH.path: text,
        _DOCUMENT_PATH: _document_page(),
        _DOCUMENT_STYLE.path: style,
        _DOCUMENT_PRELOAD_STYLE.path: style,
        _DOCUMENT_PRELOAD_SCRIPT.path: script,
        _DOCUMENT_PRELOAD_IMAGE.path: image,
        _DOCUMENT_PRELOAD_FONT.path: font,
        _DOCUMENT_SCRIPT.path: script,
        _DOCUMENT_IMAGE_SMALL.path: image,
        _DOCUMENT_IMAGE.path: image,
        _DOCUMENT_STYLE_BODY.path: style,
        _DOCUMENT_SCRIPT_DEFERRED.path: script,
        _FAVICON.path: image,
        _FAVICON_NOT_ASKED_PATH: text,
        _DOCUMENT_FORMS_PATH: _page(_form(_FORM_GET.path.partition("?")[0], method="get")),
        _FORM_GET.path: _page(_form(_FORM_TEXT.path, enctype="text/plain")),
        _FORM_TEXT.path: _page(_form(cross_site + _FORM_CROSS_SITE.path)),
        _FORM_CROSS_SITE.path: _link_page(page + _HISTORY_BACK.path),
        _HISTORY_BACK.path: _page(_link(_HISTORY_NEXT_PATH), script=_HISTORY_BACK_SCRIPT),
        _HISTORY_NEXT_PATH: _page(script=_HISTORY_NEXT_SCRIPT),
        _HISTORY_COOKIE_SET_PATH: _case_response(
            _TEXT, b"set\n", ("Set-Cookie", f"{_COOKIE}; Path={_HISTORY_BACK.path}")
        ),
        _HISTORY_COOKIE_CLEAR_PATH: _case_response(
            _TEXT, b"cleared\n", ("Set-Cookie", f"{_COOKIE.partition('=')[0]}=; Max-Age=0; Path={_HISTORY_BACK.path}")
        ),
        _DOCUMENT_DONE_PATH: done,
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
        **_document_responses(page, same_site, cross_site, done, script, image, text),
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
