"""Building a request: what a caller's arguments and a Persona make of it, with no I/O.

Building is apart from waiting, so that every API that makes requests shares it. See docs/persona.md for what comes from
where.
"""

import dataclasses
from collections.abc import Iterable, Mapping
from typing import TypedDict

from mitto._body import FileFields, QueryParams, RequestData, encode_body, encode_query
from mitto._persona import Persona, RequestDefaults, RequestKind
from mitto._timeout import Timeout, to_timeout

__all__ = ["BodyOptions", "BuiltRequest", "RequestHeaders", "RequestOptions", "build_request"]

type RequestHeaders = Mapping[str, str | None]
"""What a caller changes about the Persona's headers, by name in any case. None removes a line of the Persona's."""

# The methods that Fetch writes in upper case however they are given. PATCH is not one of them, so a page's "patch" goes
# out as it is written, in Chrome and in Session.request().
_METHODS_FETCH_NORMALIZES = frozenset({"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"})

# Fetch refuses a body with these, so Chrome never sends one.
_METHODS_WITHOUT_A_BODY = frozenset({"GET", "HEAD"})

# The native core sends a caller's Referer from its own option, and refuses one among the headers.
_REFERER = "referer"

_CONTENT_TYPE = "Content-Type"


class BodyOptions(TypedDict, total=False):
    """The keyword arguments that make a body. Only one of them makes it, but for `data` with text fields beside `files`.

    Attributes:
        data: Bytes go as they are, with no Content-Type of their own. Text goes as UTF-8, and a mapping is encoded as
            URLSearchParams does. Chrome's Content-Type for those depends on the Request Kind: a fetch says what a page's
            script would send, and a navigation what a form would.
        json: A value that goes as JSON.stringify writes it, with a Content-Type of application/json. A float keeps
            Python's text, so 1.0 stays "1.0". None sends no JSON.
        files: Files by field name, or pairs that can repeat a name, which make a multipart body as Blink's FormData does.
            A file is its content in bytes, (file name, content) or (file name, content, media type). With no file name it
            is Blink's "blob", and with no media type it is application/octet-stream, since a type is not guessed. Text
            fields in a mapping in `data` come before the files.
    """

    data: RequestData | None
    json: object
    files: FileFields | None


class RequestOptions(BodyOptions, total=False):
    """The keyword arguments that each method of a Session takes, with those of BodyOptions.

    Attributes:
        params: The query to add to the URL, encoded as URLSearchParams does. A query the URL already has stays as it is,
            and comes first.
        headers: A header replaces the Persona's line of that name in place, which keeps the Persona's spelling of the
            name. Other names follow the Persona's lines, before what //net adds. None removes a Persona's line, and does
            nothing to what //net adds. A line given here stays as it is on each hop, and what `initiator` would make of it
            no longer applies, so a Referer goes as it is given, to every hop. //net still decides where a Referer goes, and
            still changes an Origin on a redirect as it does in Chrome. A Cookie goes only if the Session holds no cookie
            for the URL, because //net writes over it otherwise. Authorization and Cookie do not follow a redirect to another
            origin, and stay gone if a later hop comes back.
        kind: A Request Kind, which decides what the Persona sends. The default is navigation for GET and fetch for other
            methods.
        initiator: The URL of the page the request is made from, which counts as a top-level page. It decides Referer,
            Origin and Sec-Fetch-Site on each hop, which SameSite cookies go, and which connections and partitioned cookies
            the request can use. Without it, a navigation counts as started by the browser, and a subresource as coming from
            the page at the root of the origin of its URL. A URL that is not valid ends the request with ERR_INVALID_ARGUMENT.
        timeout: A number is the total time in seconds, and a Timeout sets each timer. None runs no timer, in place of the
            Session's default. The default is the Session's.
        follow_redirects: False makes a redirect the response, with empty content, because the network stack does not read
            the body of a redirect. A redirect to a URL that is not valid still ends the request with a RequestError. The
            default is True.
    """

    params: QueryParams | None
    headers: RequestHeaders | None
    kind: RequestKind | str | None
    initiator: str | None
    timeout: float | Timeout | None
    follow_redirects: bool


class BuiltRequest(RequestDefaults):
    """A whole request, as the keyword arguments of the native Session's start() and request().

    Attributes:
        url: The URL to fetch, with the query from `params`.
        method: The HTTP method to send.
        referrer: The Referer that a caller gives, or None to leave it to `fetch`, or to send none if a caller removed it.
        timeout: The timers of the request.
        follow_redirects: Whether a redirect is followed, or is the response.
        body: The bytes to send as the request's body, or None for no body.
    """

    url: str
    method: str
    referrer: str | None
    timeout: Timeout
    follow_redirects: bool
    body: bytes | None


def build_request(
    persona: Persona,
    method: str,
    url: str,
    *,
    params: QueryParams | None = None,
    headers: RequestHeaders | None = None,
    kind: RequestKind | str | None = None,
    initiator: str | None = None,
    timeout: float | Timeout | None = None,
    follow_redirects: bool = True,
    data: RequestData | None = None,
    json: object = None,
    files: FileFields | None = None,
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
        initiator: See RequestOptions.
        timeout: See RequestOptions. A Session's default is the caller's to put in place of a `timeout` that was not given.
        follow_redirects: See RequestOptions.
        data: See BodyOptions.
        json: See BodyOptions.
        files: See BodyOptions.

    Raises:
        TypeError: If `data` is of no type that makes a body, or `json` holds a value that JSON has no form of.
        ValueError: If `kind` is not a Request Kind, two names in `headers` differ only in case or `timeout` is not a
            positive number of at most 2,000,000 seconds. Also if the arguments of a body do not go together, `json` holds
            a NaN or an infinity, or a body goes with GET or HEAD or with a Request Kind that Chrome sends none with.
    """
    method = method.upper() if method.upper() in _METHODS_FETCH_NORMALIZES else method
    url = _with_query(url, encode_query(params)) if params is not None else url
    if kind is None:
        kind = RequestKind.NAVIGATION if method == "GET" else RequestKind.FETCH
    request_kind = RequestKind(kind)
    body = encode_body(data, json, files)
    if body is not None and method in _METHODS_WITHOUT_A_BODY:
        raise ValueError(f"Fetch refuses a body with {method}, so Chrome never sends one")
    defaults = persona.request_defaults(request_kind, with_body=body is not None)
    changes = _changes_by_name(headers or {})
    fetch = dataclasses.replace(defaults["fetch"], initiator=initiator, fixed_headers=tuple(changes))
    _, referrer = changes.pop(_REFERER, (_REFERER, None))
    if body is not None:
        # This takes the place of the Persona's line, as a caller's would. Having no type removes the line, while a type
        # that a caller gives for bytes still goes where Chrome puts it.
        changes.setdefault(_CONTENT_TYPE.lower(), (_CONTENT_TYPE, persona.content_type(request_kind, body)))
    return BuiltRequest(
        url=url,
        method=method,
        headers=_merge_headers(defaults["headers"], changes),
        priority=defaults["priority"],
        priority_incremental=defaults["priority_incremental"],
        fetch=fetch,
        referrer=referrer,
        timeout=to_timeout(timeout),
        follow_redirects=follow_redirects,
        body=None if body is None else body.content,
    )


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
