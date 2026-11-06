"""Building a request: what a caller's arguments and a Persona make of it, with no I/O.

Building is apart from waiting, so that every API that makes requests shares it. See docs/persona.md for what comes from
where.
"""

from collections.abc import Iterable, Mapping
from typing import TypedDict, cast
from urllib.parse import quote_plus

from mitto._persona import Persona, RequestDefaults, RequestKind
from mitto._timeout import Timeout, to_timeout

__all__ = ["BuiltRequest", "QueryParams", "RequestHeaders", "RequestOptions", "build_request", "encode_query"]

type QueryParams = Mapping[str, str] | Iterable[tuple[str, str]]
"""A query as names and values. Pairs can repeat a name."""

type RequestHeaders = Mapping[str, str | None]
"""What a caller changes about the Persona's headers, by name in any case. None removes a line of the Persona's."""

# The methods that Fetch writes in upper case however they are given. PATCH is not one of them, so a page's "patch" goes
# out as it is written, in Chrome and in Session.request().
_METHODS_FETCH_NORMALIZES = frozenset({"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"})

# The native core sends a Referer from its own option, and refuses one among the headers.
_REFERER = "referer"


class RequestOptions(TypedDict, total=False):
    """The keyword arguments that each method of a Session takes.

    Attributes:
        params: The query to add to the URL, encoded as URLSearchParams does. A query the URL already has stays as it is,
            and comes first.
        headers: A header replaces the Persona's line of that name in place, which keeps the Persona's spelling of the
            name. Other names follow the Persona's lines, before what //net adds. None removes a Persona's line, and does
            nothing to what //net adds. Referer counts as a line of the Persona's, although //net decides where it goes.
        kind: A Request Kind, which decides what the Persona sends. The default is navigation for GET and fetch for other
            methods.
        timeout: A number is the total time in seconds, and a Timeout sets each timer. None runs no timer, in place of the
            Session's default. The default is the Session's.
        follow_redirects: False makes a redirect the response, with empty content, because the network stack does not read
            the body of a redirect. A redirect to a URL that is not valid still ends the request with a RequestError. The
            default is True.
    """

    params: QueryParams | None
    headers: RequestHeaders | None
    kind: RequestKind | str | None
    timeout: float | Timeout | None
    follow_redirects: bool


class BuiltRequest(RequestDefaults):
    """A whole request, as the keyword arguments of the native Session's start() and request().

    Attributes:
        url: The URL to fetch, with the query from `params`.
        method: The HTTP method to send.
        timeout: The timers of the request.
        follow_redirects: Whether a redirect is followed, or is the response.
    """

    url: str
    method: str
    timeout: Timeout
    follow_redirects: bool


def build_request(
    persona: Persona,
    method: str,
    url: str,
    *,
    params: QueryParams | None = None,
    headers: RequestHeaders | None = None,
    kind: RequestKind | str | None = None,
    timeout: float | Timeout | None = None,
    follow_redirects: bool = True,
) -> BuiltRequest:
    """Return what to ask the native core for, given a caller's arguments as described in RequestOptions.

    Args:
        persona: The Persona to present.
        method: The HTTP method. Those that Fetch normalizes are sent in upper case however they are given, as Chrome
            does, and any other is sent as it is written.
        url: The URL to fetch.
        params: See RequestOptions.
        headers: See RequestOptions.
        kind: See RequestOptions.
        timeout: See RequestOptions. A Session's default is the caller's to put in place of a `timeout` that was not given.
        follow_redirects: See RequestOptions.

    Raises:
        ValueError: If `kind` is not a Request Kind, two names in `headers` differ only in case or `timeout` is not a
            positive number of at most 2,000,000 seconds.
    """
    method = method.upper() if method.upper() in _METHODS_FETCH_NORMALIZES else method
    url = _with_query(url, encode_query(params)) if params is not None else url
    if kind is None:
        kind = RequestKind.NAVIGATION if method == "GET" else RequestKind.FETCH
    defaults = persona.request_defaults(RequestKind(kind), url)
    changes = _changes_by_name(headers or {})
    _, referrer = changes.pop(_REFERER, (_REFERER, defaults["referrer"]))
    return BuiltRequest(
        url=url,
        method=method,
        headers=_merge_headers(defaults["headers"], changes),
        priority=defaults["priority"],
        priority_incremental=defaults["priority_incremental"],
        referrer=referrer,
        timeout=to_timeout(timeout),
        follow_redirects=follow_redirects,
    )


def encode_query(params: QueryParams) -> str:
    """Return `params` as the text of a query, as URLSearchParams.toString() does."""
    pairs = cast(Mapping[str, str], params).items() if isinstance(params, Mapping) else params
    return "&".join(f"{_encode_component(name)}={_encode_component(value)}" for name, value in pairs)


def _encode_component(text: str) -> str:
    # Web IDL turns a lone surrogate into U+FFFD before URLSearchParams sees it, where encoding as UTF-8 would fail.
    well_formed = text.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace")
    # The application/x-www-form-urlencoded set keeps "*" and encodes "~", which quote() does the other way round.
    return quote_plus(well_formed, safe="*").replace("~", "%7E")


def _with_query(url: str, query: str) -> str:
    """Return `url` with `query` added, before any fragment. The text of the URL does not change otherwise."""
    if not query:
        return url
    before_fragment, hash_sign, fragment = url.partition("#")
    if "?" not in before_fragment:
        separator = "?"
    elif before_fragment.endswith(("?", "&")):
        separator = ""
    else:
        separator = "&"
    return f"{before_fragment}{separator}{query}{hash_sign}{fragment}"


def _changes_by_name(headers: RequestHeaders) -> dict[str, tuple[str, str | None]]:
    """Return a caller's (name, value) pairs by lower-case name.

    Raises:
        ValueError: If two names differ only in case, since neither is more right than the other.
    """
    changes = {name.lower(): (name, value) for name, value in headers.items()}
    if len(changes) != len(headers):
        raise ValueError(f"headers holds names that differ only in case: {list(headers)}")
    return changes


def _merge_headers(
    lines: Iterable[tuple[str, str]], changes: dict[str, tuple[str, str | None]]
) -> tuple[tuple[str, str], ...]:
    """Return the Persona's `lines` with a caller's `changes` made, followed by the names they do not hold.

    This takes what it uses out of `changes`.
    """
    merged: list[tuple[str, str]] = []
    for name, value in lines:
        _, new_value = changes.pop(name.lower(), (name, value))
        if new_value is not None:
            merged.append((name, new_value))
    merged += [(name, value) for name, value in changes.values() if value is not None]
    return tuple(merged)
