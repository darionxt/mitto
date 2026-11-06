"""Replaying a Recording Case: what a Session asks for to produce it, as far as the public API reaches.

A replay does what a caller of the public API can do today, and what it cannot do shows up as gaps. So when a Session
gains something a case needs, the replay here passes it on, and those gaps leave the known-gaps list. See docs/parity.md.
"""

from dataclasses import dataclass, field

from mitto import Session
from mitto._native import PrivateSettings
from mitto._request import BodyOptions
from tools.capture import (
    CASES,
    COOKIE_SET_PATH,
    CROSS_SITE_HOST,
    FORMS_PATH,
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
    RequestKind,
    Run,
)

_PATHS = {case.name: case.path for case in CASES}
_POST = "POST"
_TEXT: BodyOptions = {"data": "text body"}


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
        body: What makes the body, from what the page's form or script gives Chrome in tools/capture/cases.py.
        page: The path of the page the request came from in tools/capture/cases.py, or nothing for what a request with no
            Initiator is.
        page_host: The name of that page, or nothing for the name the case's run starts at.
    """

    kind: RequestKind
    method: str = "GET"
    host: str | None = None
    start: str | None = None
    before: tuple[str, ...] = ()
    body: BodyOptions = field(default_factory=BodyOptions)
    page: str | None = None
    page_host: str | None = None


def _hops(prefix: str, kind: RequestKind, page: str) -> dict[str, Replay]:
    return {f"{prefix}_{hop}": Replay(kind, start=f"{prefix}_1", page=page) for hop in range(1, 5)}


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
    "navigation_form_urlencoded": Replay(
        RequestKind.NAVIGATION, _POST, body={"data": {"name": "mitto", "a b&c": "d e&f+g"}}, page=FORMS_PATH
    ),
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
    "fetch_post_stream": Replay(RequestKind.FETCH, _POST, page=START_PATH),
    "script_same_site": Replay(RequestKind.SCRIPT, host=SAME_SITE_HOST, page=START_PATH),
    "script_cross_site": Replay(RequestKind.SCRIPT, host=CROSS_SITE_HOST, page=START_PATH),
    "image_same_site": Replay(RequestKind.IMAGE, host=SAME_SITE_HOST, page=START_PATH),
    "image_cross_site": Replay(RequestKind.IMAGE, host=CROSS_SITE_HOST, page=START_PATH),
    "cookie": Replay(RequestKind.FETCH, before=(COOKIE_SET_PATH,), page=START_PATH),
    "revalidation": Replay(RequestKind.FETCH, before=(_PATHS["revalidation"],), page=START_PATH),
    **_hops("redirect_navigation", RequestKind.NAVIGATION, _PATHS["navigation_form_multipart"]),
    **_hops("redirect_fetch", RequestKind.FETCH, START_PATH),
    "plain_http_navigation": Replay(RequestKind.NAVIGATION),
    "plain_http_fetch": Replay(RequestKind.FETCH, page=_PATHS["plain_http_navigation"]),
    "plain_http_script": Replay(RequestKind.SCRIPT, page=_PATHS["plain_http_navigation"]),
    "plain_http_image": Replay(RequestKind.IMAGE, page=_PATHS["plain_http_navigation"]),
    "proxy_connect": Replay(RequestKind.NAVIGATION, start="proxy_navigation"),
    "proxy_navigation": Replay(RequestKind.NAVIGATION),
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

_RUNS = {(case.name, _PROTOCOLS[run.route]): run for run in RUNS for case in run.cases}

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
    for path in steps.before:
        session.get(server.url(run.route, host=run.host, path=path), kind=kind, initiator=initiator)
    path = _PATHS[steps.start or case]
    url = server.url(run.route, host=steps.host or run.host, path=path)
    session.request(steps.method, url, kind=kind, initiator=initiator, **steps.body)
