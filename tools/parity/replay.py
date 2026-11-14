"""Replaying a Recording Case: what a Session asks for to produce it, as far as the public API reaches.

A replay does what a caller of the public API can do today, and what it cannot do shows up as gaps. So when a Session
gains something a case needs, the replay here passes it on, and those gaps leave the known-gaps list. See docs/parity.md.
"""

from dataclasses import dataclass, field

from mitto import RequestKind, Session
from mitto._native import PrivateSettings
from mitto._request import BodyOptions
from tools.capture import (
    CASES,
    CLIENT_HINTS_PLAIN_HTTP_START_PATH,
    COOKIE_SET_PATH,
    CREDENTIALS_COOKIE_SET_PATH,
    CROSS_SITE_HOST,
    DOCUMENT_CASES,
    DOCUMENT_PATH,
    DOCUMENT_RUNS,
    DOCUMENTS_FORMS_PATH,
    DOCUMENTS_FRAMES_PATH,
    DOCUMENTS_START_PATH,
    FORMS_PATH,
    HINTS_ASKED,
    HISTORY_COOKIE_SET_PATH,
    HTTP1,
    HTTP2,
    HTTP3,
    PLAIN_HTTP,
    PROXIED_HTTP1,
    PROXIED_HTTP2,
    RUNS,
    SAME_SITE_HOST,
    START_PATH,
    CaptureServer,
    Run,
)

_PATHS = {case.name: case.path for case in (*CASES, *DOCUMENT_CASES)}
_POST = "POST"
_TEXT: BodyOptions = {"data": "text body"}
_JSON: BodyOptions = {"json": {"json": "body"}}
_FORM_FIELDS = {"name": "mitto", "a b&c": "d e&f+g"}


@dataclass(frozen=True, slots=True)
class Replay:
    """What a Session asks for to produce one Recording Case.

    Attributes:
        kind: The Request Kind to ask for.
        method: The HTTP method.
        host: The name to ask for, or nothing for the name the case's run starts at.
        start: The name of the case to ask for, if asking for that is what makes this one: the first hop of a redirect
            chain for a later hop, and what goes through a tunnel for what opens the tunnel.
        before: The paths to ask for first, on the same Session and with the same kind.
        asks: The paths to navigate to first, on the same Session and from no page, so that their origin asks for hints,
            or so that they set the cookies that the case carries.
        asks_host: The name to navigate to for `asks`, or nothing for the name the case's run starts at.
        body: What makes the body, from what the page's form or script gives Chrome in tools/capture/cases.py.
        page: The path of the page the request came from in tools/capture/cases.py, or nothing for what a request with no
            Initiator is.
        page_host: The name of that page, or nothing for the name the case's run starts at.
        cache: Whether the Session has a cache in a directory, as the Chrome that recorded the case had, for a case that
            needs one to show what it is about.
        made_by_a_page: The issue that makes a Page make this request, for a case that only a page makes in a way that a
            caller cannot say, such as a navigation that no user started. No check replays it until that issue
            replays it through a Page and clears this.
    """

    kind: RequestKind
    method: str = "GET"
    host: str | None = None
    start: str | None = None
    before: tuple[str, ...] = ()
    asks: tuple[str, ...] = ()
    asks_host: str | None = None
    body: BodyOptions = field(default_factory=BodyOptions)
    page: str | None = None
    page_host: str | None = None
    cache: bool = False
    made_by_a_page: int | None = None


def _hops(prefix: str, kind: RequestKind, page: str) -> dict[str, Replay]:
    return {f"{prefix}_{hop}": Replay(kind, start=f"{prefix}_1", page=page) for hop in range(1, 5)}


def _client_hints_replays() -> dict[str, Replay]:
    """Return what produces each case of the client hints runs, after the page's origin has asked."""
    first = _PATHS["client_hints_first"]
    asks = (first,)
    replays = {
        "client_hints_first": Replay(RequestKind.NAVIGATION),
        "client_hints_fetch_post": Replay(RequestKind.FETCH, _POST, body=_TEXT, asks=asks, page=first),
        "client_hints_navigation": Replay(RequestKind.NAVIGATION, asks=asks, page=first),
        "client_hints_navigation_same_site": Replay(
            RequestKind.NAVIGATION, host=SAME_SITE_HOST, asks=asks, page=_PATHS["client_hints_navigation"]
        ),
        "client_hints_navigation_cross_site": Replay(
            RequestKind.NAVIGATION,
            host=CROSS_SITE_HOST,
            asks=asks,
            page=_PATHS["client_hints_navigation_same_site"],
            page_host=SAME_SITE_HOST,
        ),
        "client_hints_navigation_back": Replay(
            RequestKind.NAVIGATION,
            asks=asks,
            page=_PATHS["client_hints_navigation_cross_site"],
            page_host=CROSS_SITE_HOST,
        ),
        "client_hints_form": Replay(
            RequestKind.NAVIGATION,
            _POST,
            body={"data": {"name": "mitto", "a b&c": "d e&f+g"}},
            asks=asks,
            page=_PATHS["client_hints_navigation_back"],
        ),
        "client_hints_critical": Replay(
            RequestKind.NAVIGATION, host=CROSS_SITE_HOST, page=HINTS_ASKED[-1].navigation.path
        ),
        "client_hints_plain_http_fetch": Replay(
            RequestKind.FETCH, asks=(CLIENT_HINTS_PLAIN_HTTP_START_PATH,), page=CLIENT_HINTS_PLAIN_HTTP_START_PATH
        ),
        "client_hints_plain_http_navigation": Replay(
            RequestKind.NAVIGATION, asks=(CLIENT_HINTS_PLAIN_HTTP_START_PATH,), page=CLIENT_HINTS_PLAIN_HTTP_START_PATH
        ),
    }
    for kind, suffix in (
        (RequestKind.SCRIPT, "script"),
        (RequestKind.IMAGE, "image"),
        (RequestKind.FETCH, "fetch"),
    ):
        replays[f"client_hints_{suffix}"] = Replay(kind, asks=asks, page=first)
        replays[f"client_hints_{suffix}_same_site"] = Replay(kind, host=SAME_SITE_HOST, asks=asks, page=first)
        replays[f"client_hints_{suffix}_cross_site"] = Replay(kind, host=CROSS_SITE_HOST, asks=asks, page=first)
    for hop in range(1, 5):
        replays[f"client_hints_redirect_fetch_{hop}"] = Replay(
            RequestKind.FETCH, start="client_hints_redirect_fetch_1", asks=asks, page=first
        )
    for hop in range(1, 6):
        replays[f"client_hints_redirect_navigation_{hop}"] = Replay(
            RequestKind.NAVIGATION,
            start="client_hints_redirect_navigation_1",
            asks=asks,
            page=_PATHS["client_hints_form"],
        )
    for asked in HINTS_ASKED:
        page_asks = (asked.path,)
        replays[asked.fetch.name] = Replay(RequestKind.FETCH, asks=page_asks, page=asked.path)
        replays[asked.post.name] = Replay(RequestKind.FETCH, _POST, body=_TEXT, asks=page_asks, page=asked.path)
        replays[asked.navigation.name] = Replay(RequestKind.NAVIGATION, asks=page_asks, page=asked.path)
    return replays


def _document_replays() -> dict[str, Replay]:
    """Return what produces each case of the documents runs, as far as a Session reaches.

    A page's own request names its page as the Initiator here, as a caller's does. A case that only a page makes in a way a
    caller cannot say, such as a worker's request, one of a frame of another site, a navigation that no user started, or
    what a document's markup asks for in Blink's order and priorities, names the issue that makes a Page make it.
    """
    start = DOCUMENTS_START_PATH
    frame = _PATHS["frame_cross_site"]
    return {
        "style": Replay(RequestKind.STYLE, page=start),
        "style_import": Replay(RequestKind.STYLE, page=_PATHS["style"]),
        "style_cross_site": Replay(RequestKind.STYLE, host=CROSS_SITE_HOST, page=start),
        "font": Replay(RequestKind.FONT, page=_PATHS["style"]),
        "font_cross_site": Replay(RequestKind.FONT, host=CROSS_SITE_HOST, page=_PATHS["style"]),
        "module": Replay(RequestKind.MODULE, page=start),
        "module_static_import": Replay(RequestKind.MODULE, page=_PATHS["module"]),
        "module_dynamic_import": Replay(RequestKind.MODULE, page=_PATHS["module"]),
        "module_cross_site": Replay(RequestKind.MODULE, host=CROSS_SITE_HOST, page=start),
        "xhr_get": Replay(RequestKind.XHR, page=start),
        "xhr_get_cross_site": Replay(RequestKind.XHR, host=CROSS_SITE_HOST, page=start),
        "xhr_post_text": Replay(RequestKind.XHR, _POST, body=_TEXT, page=start),
        "xhr_get_sync": Replay(RequestKind.XHR, page=start, made_by_a_page=283),
        "xhr_post_text_sync": Replay(RequestKind.XHR, _POST, body=_TEXT, page=start, made_by_a_page=283),
        "worker": Replay(RequestKind.WORKER, page=start),
        "worker_import_scripts": Replay(RequestKind.SCRIPT, page=_PATHS["worker"], made_by_a_page=289),
        "worker_fetch": Replay(RequestKind.FETCH, page=_PATHS["worker"], made_by_a_page=289),
        "worker_module": Replay(RequestKind.WORKER, page=start),
        "worker_blob_fetch": Replay(RequestKind.FETCH, page=start, made_by_a_page=289),
        "beacon_text": Replay(RequestKind.BEACON, _POST, body=_TEXT, page=start),
        "beacon_blob": Replay(RequestKind.BEACON, _POST, body={"data": b"blob body"}, page=start),
        "beacon_form_data": Replay(
            RequestKind.BEACON, _POST, body={"data": {"name": "mitto"}, "files": {}}, page=start
        ),
        "beacon_text_cross_site": Replay(RequestKind.BEACON, _POST, host=CROSS_SITE_HOST, body=_TEXT, page=start),
        "fetch_later": Replay(RequestKind.FETCH, page=start),
        "preflight": Replay(
            RequestKind.FETCH, _POST, host=CROSS_SITE_HOST, start="fetch_post_json_cross_site", body=_JSON, page=start
        ),
        "fetch_post_json_cross_site": Replay(RequestKind.FETCH, _POST, host=CROSS_SITE_HOST, body=_JSON, page=start),
        "fetch_credentials_omit": Replay(
            RequestKind.FETCH,
            host=SAME_SITE_HOST,
            asks=(CREDENTIALS_COOKIE_SET_PATH,),
            asks_host=SAME_SITE_HOST,
            page=start,
        ),
        "fetch_credentials_same_origin": Replay(
            RequestKind.FETCH,
            host=SAME_SITE_HOST,
            asks=(CREDENTIALS_COOKIE_SET_PATH,),
            asks_host=SAME_SITE_HOST,
            page=start,
        ),
        "fetch_credentials_include": Replay(
            RequestKind.FETCH,
            host=SAME_SITE_HOST,
            asks=(CREDENTIALS_COOKIE_SET_PATH,),
            asks_host=SAME_SITE_HOST,
            page=start,
        ),
        "fetch_no_cors": Replay(RequestKind.FETCH, host=CROSS_SITE_HOST, page=start),
        "navigation_location": Replay(RequestKind.NAVIGATION, page=start, made_by_a_page=279),
        "navigation_click": Replay(RequestKind.NAVIGATION, page=_PATHS["navigation_location"], made_by_a_page=279),
        "navigation_meta_refresh": Replay(RequestKind.NAVIGATION, page=_PATHS["navigation_click"], made_by_a_page=279),
        "redirect_location_1": Replay(
            RequestKind.NAVIGATION,
            start="redirect_location_1",
            page=_PATHS["navigation_meta_refresh"],
            made_by_a_page=279,
        ),
        "redirect_location_2": Replay(
            RequestKind.NAVIGATION,
            start="redirect_location_1",
            page=_PATHS["navigation_meta_refresh"],
            made_by_a_page=279,
        ),
        "frame_same_origin": Replay(RequestKind.IFRAME, page=DOCUMENTS_FRAMES_PATH),
        "frame_same_site": Replay(RequestKind.IFRAME, host=SAME_SITE_HOST, page=DOCUMENTS_FRAMES_PATH),
        "frame_cross_site": Replay(RequestKind.IFRAME, host=CROSS_SITE_HOST, page=DOCUMENTS_FRAMES_PATH),
        "frame_script": Replay(
            RequestKind.SCRIPT, host=CROSS_SITE_HOST, page=frame, page_host=CROSS_SITE_HOST, made_by_a_page=285
        ),
        "frame_image": Replay(
            RequestKind.IMAGE, host=CROSS_SITE_HOST, page=frame, page_host=CROSS_SITE_HOST, made_by_a_page=285
        ),
        "frame_fetch": Replay(
            RequestKind.FETCH, host=CROSS_SITE_HOST, page=frame, page_host=CROSS_SITE_HOST, made_by_a_page=285
        ),
        # The page of the frames sets a cookie of each SameSite for these two.
        "frame_nested": Replay(
            RequestKind.IFRAME, asks=(DOCUMENTS_FRAMES_PATH,), page=frame, page_host=CROSS_SITE_HOST
        ),
        "frame_nested_fetch": Replay(
            RequestKind.FETCH, asks=(DOCUMENTS_FRAMES_PATH,), page=_PATHS["frame_nested"], made_by_a_page=285
        ),
        "document_style": Replay(RequestKind.STYLE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_preload_style": Replay(RequestKind.STYLE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_preload_script": Replay(RequestKind.SCRIPT, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_preload_image": Replay(RequestKind.IMAGE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_preload_font": Replay(RequestKind.FONT, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_script": Replay(RequestKind.SCRIPT, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_image_small": Replay(RequestKind.IMAGE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_image": Replay(RequestKind.IMAGE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_style_body": Replay(RequestKind.STYLE, page=DOCUMENT_PATH, made_by_a_page=284),
        "document_script_deferred": Replay(RequestKind.SCRIPT, page=DOCUMENT_PATH, made_by_a_page=284),
        "favicon": Replay(RequestKind.FAVICON, page=DOCUMENT_PATH),
        "navigation_form_get": Replay(RequestKind.NAVIGATION, page=DOCUMENTS_FORMS_PATH),
        "navigation_form_text": Replay(
            RequestKind.NAVIGATION,
            _POST,
            body={"data": "name=mitto\r\na b&c=d e&f+g\r\n"},
            page=_PATHS["navigation_form_get"],
        ),
        "navigation_form_cross_site": Replay(
            RequestKind.NAVIGATION,
            _POST,
            host=CROSS_SITE_HOST,
            body={"data": _FORM_FIELDS},
            page=_PATHS["navigation_form_text"],
        ),
        # The traversal goes back to an entry that the form of the other site made, and carries what the next page set.
        "navigation_history_back": Replay(
            RequestKind.NAVIGATION,
            asks=(HISTORY_COOKIE_SET_PATH,),
            page=_PATHS["navigation_form_cross_site"],
            page_host=CROSS_SITE_HOST,
            made_by_a_page=280,
        ),
    }


REPLAYS = {
    RequestKind.NAVIGATION.value: Replay(RequestKind.NAVIGATION),
    RequestKind.FETCH.value: Replay(RequestKind.FETCH),
    RequestKind.SCRIPT.value: Replay(RequestKind.SCRIPT),
    RequestKind.IMAGE.value: Replay(RequestKind.IMAGE),
    "navigation_link_same_origin": Replay(RequestKind.NAVIGATION, page=START_PATH),
    "navigation_link_same_site": Replay(
        RequestKind.NAVIGATION, host=SAME_SITE_HOST, page=_PATHS["navigation_link_same_origin"]
    ),
    "navigation_link_cross_site": Replay(
        RequestKind.NAVIGATION,
        host=CROSS_SITE_HOST,
        page=_PATHS["navigation_link_same_site"],
        page_host=SAME_SITE_HOST,
    ),
    "navigation_form_urlencoded": Replay(RequestKind.NAVIGATION, _POST, body={"data": _FORM_FIELDS}, page=FORMS_PATH),
    "navigation_form_multipart": Replay(
        RequestKind.NAVIGATION,
        _POST,
        body={
            "data": {"name": "mitto", 'a "b"': "d e&f+g"},
            "files": {"file": ('file "name".txt', b"file text\n", "text/plain")},
        },
        page=_PATHS["navigation_form_urlencoded"],
    ),
    "fetch_get_same_site": Replay(RequestKind.FETCH, host=SAME_SITE_HOST, page=START_PATH),
    "fetch_get_cross_site": Replay(RequestKind.FETCH, host=CROSS_SITE_HOST, page=START_PATH),
    "fetch_post_text_same_origin": Replay(RequestKind.FETCH, _POST, body=_TEXT, page=START_PATH),
    "fetch_post_text_cross_site": Replay(RequestKind.FETCH, _POST, host=CROSS_SITE_HOST, body=_TEXT, page=START_PATH),
    "fetch_post_json_same_origin": Replay(RequestKind.FETCH, _POST, body={"json": {"json": "body"}}, page=START_PATH),
    "fetch_post_stream": Replay(RequestKind.FETCH, _POST, body={"data": [b"stream body"]}, page=START_PATH),
    "script_same_site": Replay(RequestKind.SCRIPT, host=SAME_SITE_HOST, page=START_PATH),
    "script_cross_site": Replay(RequestKind.SCRIPT, host=CROSS_SITE_HOST, page=START_PATH),
    "image_same_site": Replay(RequestKind.IMAGE, host=SAME_SITE_HOST, page=START_PATH),
    "image_cross_site": Replay(RequestKind.IMAGE, host=CROSS_SITE_HOST, page=START_PATH),
    "cookie": Replay(RequestKind.FETCH, before=(COOKIE_SET_PATH,), page=START_PATH),
    "revalidation": Replay(RequestKind.FETCH, before=(_PATHS["revalidation"],), page=START_PATH, cache=True),
    **_hops("redirect_navigation", RequestKind.NAVIGATION, _PATHS["navigation_form_multipart"]),
    **_hops("redirect_fetch", RequestKind.FETCH, START_PATH),
    "plain_http_navigation": Replay(RequestKind.NAVIGATION),
    "plain_http_fetch": Replay(RequestKind.FETCH, page=_PATHS["plain_http_navigation"]),
    "plain_http_script": Replay(RequestKind.SCRIPT, page=_PATHS["plain_http_navigation"]),
    "plain_http_image": Replay(RequestKind.IMAGE, page=_PATHS["plain_http_navigation"]),
    "proxy_connect": Replay(RequestKind.NAVIGATION, start="proxy_navigation"),
    "proxy_navigation": Replay(RequestKind.NAVIGATION),
    **_client_hints_replays(),
    **_document_replays(),
}
"""How to replay each Recording Case, by its name."""

_PROTOCOLS = {
    HTTP1: HTTP1,
    HTTP2: HTTP2,
    HTTP3: HTTP3,
    PLAIN_HTTP: HTTP1,
    PROXIED_HTTP1: HTTP1,
    PROXIED_HTTP2: HTTP2,
}
"""The protocol of what comes in by each route."""

_RUNS = {(case.name, _PROTOCOLS[run.route]): run for run in (*RUNS, *DOCUMENT_RUNS) for case in run.cases}

_PROXIED_ROUTES = frozenset({PROXIED_HTTP1, PROXIED_HTTP2})


def protocol_of(route: str) -> str:
    """Return the protocol of what comes in by a route."""
    return _PROTOCOLS[route]


def run_of(case: str, protocol: str) -> Run | None:
    """Return the run that records a Recording Case over a protocol, or None if no run does."""
    return _RUNS.get((case, protocol))


def proxy_of(server: CaptureServer, run: Run) -> str | None:
    """Return the proxy that the Chrome of a run went through, or None if it went directly."""
    return server.proxy_url if run.route in _PROXIED_ROUTES else None


def made_by_a_page(case: str) -> int | None:
    """Return the issue that makes a Page make a Recording Case, until which no check replays it, or None."""
    return REPLAYS[case].made_by_a_page


def needs_cache(case: str) -> bool:
    """Return whether a Session that replays a Recording Case needs a cache in a directory."""
    return REPLAYS[case].cache


def session_settings(server: CaptureServer, run: Run) -> PrivateSettings:
    """Return what a Session needs that the public API keeps private, to reach a server as the Chrome of a run did."""
    # The proxy is what finds the name, as in CaptureServer.chrome_flags().
    finds_its_own_names = run.host is None or run.route in _PROXIED_ROUTES
    return PrivateSettings(
        ignore_certificate_errors_spki_list=[server.spki_hash],
        origins_to_force_quic_on=server.origins_to_force_quic_on(run),
        host_resolver_rules="" if finds_its_own_names else server.host_resolver_rules,
    )


def replay(session: Session, server: CaptureServer, run: Run, case: str) -> None:
    """Make the requests that produce a Recording Case of a run.

    Raises:
        RequestError: A request got no response.
    """
    steps = REPLAYS[case]
    kind = steps.kind.value
    initiator = None
    if steps.page is not None:
        initiator = server.url(run.route, host=steps.page_host or run.host, path=steps.page)
    for path in steps.asks:
        session.get(server.url(run.route, host=steps.asks_host or run.host, path=path), kind=RequestKind.NAVIGATION)
    for path in steps.before:
        session.get(server.url(run.route, host=run.host, path=path), kind=kind, initiator=initiator)
    path = _PATHS[steps.start or case]
    url = server.url(run.route, host=steps.host or run.host, path=path)
    session.request(steps.method, url, kind=kind, initiator=initiator, **steps.body)
