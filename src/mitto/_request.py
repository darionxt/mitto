"""Building a request: what a caller's arguments and a Persona make of it, with no I/O but asking a file for its size.

Building is apart from waiting, so that every API that makes requests shares it. See docs/persona.md for what comes from
where.
"""

import dataclasses
from collections.abc import AsyncIterable, Buffer, Iterable, Mapping
from typing import TypedDict, cast

from mitto._arguments import require_type
from mitto._body import (
    BodyEncoding,
    EncodedBody,
    FileFields,
    QueryParams,
    RequestData,
    StreamBody,
    encode_body,
    encode_query,
    with_declared_size,
)
from mitto._native import encode_url
from mitto._persona import Persona, RequestDefaults, RequestKind
from mitto._timeout import Timeout, to_timeout

__all__ = ["AsyncRequestOptions", "BodyOptions", "BuiltRequest", "RequestHeaders", "RequestOptions", "build_request"]

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
_CONTENT_LENGTH = "content-length"
_SEC_FETCH_MODE = "Sec-Fetch-Mode"
_ORIGIN = "Origin"
_ORIGIN_PLACEHOLDER = "<origin>"

_CREDENTIALS = frozenset({"omit", "same-origin", "include"})
_MODES = frozenset({"cors", "no-cors", "same-origin"})
_NO_CORS = "no-cors"
_CORS = "cors"
_INCLUDE = "include"
_SAME_ORIGIN = "same-origin"

# What Fetch allows a request of mode no-cors to have as its method.
_NO_CORS_METHODS = frozenset({"GET", "HEAD", "POST"})
# The MIME types of a CORS-safelisted Content-Type, the only one that a request in mode no-cors takes.
_NO_CORS_CONTENT_TYPES = frozenset({"application/x-www-form-urlencoded", "multipart/form-data", "text/plain"})

# The kinds that navigate, whose mode is navigate and whose credentials are always included.
_NAVIGATIONS = frozenset({RequestKind.NAVIGATION, RequestKind.IFRAME})


class BodyOptions(TypedDict, total=False):
    """The keyword arguments that make a body. Only one of them makes it, but for `data` with text fields beside `files`.

    Attributes:
        data: Bytes go as they are, with no Content-Type of their own. Text goes as UTF-8, and a mapping is encoded as
            URLSearchParams does. Chrome's Content-Type for those depends on the Request Kind: a fetch says what a page's
            script would send, and a navigation what a form would. A file object in binary mode or an iterable of bytes is
            a stream, with no Content-Type of its own, which reads it only as the network stack asks for more, in the
            thread that waits for the response. A stream goes once, so any redirect but a 303 raises a RequestError, as
            Fetch fails it. A file that can seek sends a Content-Length, for what it holds from where it stands. Without a
            known size a stream goes as Chrome sends a page's ReadableStream, which it refuses over HTTP/1.1, so that
            raises a RequestError named ERR_ALPN_NEGOTIATION_FAILED. A `Content-Length` in `headers` gives the size, and the
            stream raises a ValueError if it does not keep to it.
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
            Origin and Sec-Fetch-Site on each hop, which SameSite cookies go, which connections and partitioned cookies the
            request can use, and for a kind other than navigation, which client hints it sends. Without it, a navigation
            counts as started by the browser, and a subresource as coming from the page at the root of the origin of its
            URL. A URL that is not valid ends the request with ERR_INVALID_ARGUMENT.
        timeout: A number is the total time in seconds, and a Timeout sets each timer. None runs no timer, in place of the
            Session's default. The default is the Session's.
        follow_redirects: False makes a redirect the response, with empty content, because the network stack does not read
            the body of a redirect. A redirect to a URL that is not valid still ends the request with a RequestError. The
            default is True.
        credentials: "omit", "same-origin" or "include", as fetch()'s `credentials` says whether cookies go and are kept,
            in place of the kind's own. A navigation's are always included, so it takes only "include".
        mode: "cors", "no-cors" or "same-origin", as fetch()'s `mode` says, in place of the kind's own, which Sec-Fetch-Mode
            says. An element in cors mode, as `crossorigin` puts it, sends Origin as a font does. A navigation takes none,
            a worker's script only "same-origin", and "no-cors" only GET, HEAD and POST.
    """

    params: QueryParams | None
    headers: RequestHeaders | None
    kind: RequestKind | str | None
    initiator: str | None
    timeout: float | Timeout | None
    follow_redirects: bool
    credentials: str | None
    mode: str | None


class AsyncRequestOptions(TypedDict, total=False):
    """The keyword arguments that each method of an AsyncSession takes, which are those of RequestOptions.

    Attributes:
        data: As in BodyOptions, or also an async iterable of bytes, which is a stream with no size unless `headers` gives
            one. A file or an iterable is read on a thread of the event loop's default executor, as the network stack asks
            for more, so one that waits for long takes up a thread until it gives something. An async iterable is read on
            the loop, and takes up nothing while it waits. Bytes in memory are copied on the loop, once, which holds it up
            for tens of milliseconds at hundreds of MiB, so a body that large is better off as a stream.
    """

    data: RequestData | AsyncIterable[Buffer] | None
    json: object
    files: FileFields | None
    params: QueryParams | None
    headers: RequestHeaders | None
    kind: RequestKind | str | None
    initiator: str | None
    timeout: float | Timeout | None
    follow_redirects: bool
    credentials: str | None
    mode: str | None


class BuiltRequest(RequestDefaults):
    """A whole request, as the keyword arguments of the native Session's start() and request().

    Attributes:
        url: The URL to fetch, with the query from `params`.
        method: The HTTP method to send.
        referrer: The Referer that a caller gives, or None to leave it to `fetch`, or to send none if a caller removed it.
        timeout: The timers of the request.
        follow_redirects: Whether a redirect is followed, or is the response.
        body: What to send as the request's body, as bytes or a stream, or None for no body.
    """

    url: str
    method: str
    referrer: str | None
    timeout: Timeout
    follow_redirects: bool
    body: bytes | StreamBody | None


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
    credentials: str | None = None,
    mode: str | None = None,
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
        credentials: See RequestOptions.
        mode: See RequestOptions.

    Raises:
        TypeError: If an argument is of a type it does not take, as Session.request() says, which the message names. A
            name or value in `params` or a mapping in `data` that is not text, a name in `headers` that is not text or a
            value in it that is neither text nor None, a file in `files` of another form and a value in `json` that JSON
            has no form of count as well.
        ValueError: If `kind` is not a Request Kind, two names in `headers` differ only in case, a Referer in them holds a
            NUL character or cannot be encoded as UTF-8, or `timeout` is not a positive number of at most 2,000,000
            seconds. Also if the arguments of a body do not go together, `json` holds a NaN or an infinity, a body goes with
            GET or HEAD or with a Request Kind that Chrome sends none with, or the `Content-Length` of a stream is not a
            number of bytes. Also if `credentials` or `mode` is none of its values, or one that the kind cannot have.
    """
    require_type("method", method, str, "text")
    require_type("url", url, str, "text")
    require_type("kind", kind, str | None, "a RequestKind, its value or None")
    require_type("initiator", initiator, str | None, "text or None")
    require_type("follow_redirects", follow_redirects, bool, "True or False")
    require_type("credentials", credentials, str | None, "text or None")
    require_type("mode", mode, str | None, "text or None")
    method = method.upper() if method.upper() in _METHODS_FETCH_NORMALIZES else method
    url = _with_query(url, encode_query(params)) if params is not None else url
    if kind is None:
        kind = RequestKind.NAVIGATION if method == "GET" else RequestKind.FETCH
    request_kind = RequestKind(kind)
    body = encode_body(data, json, files)
    if body is not None and method in _METHODS_WITHOUT_A_BODY:
        raise ValueError(f"Fetch refuses a body with {method}, so Chrome never sends one")
    defaults = _with_fetch(
        persona.request_defaults(request_kind, with_body=body is not None), request_kind, method, credentials, mode
    )
    changes = _changes_by_name(headers)
    if isinstance(body, StreamBody):
        body = _with_size_from_headers(body, changes)
    fetch = dataclasses.replace(defaults["fetch"], initiator=initiator, fixed_headers=tuple(changes))
    _, referrer = changes.pop(_REFERER, (_REFERER, None))
    if referrer is not None:
        # The native core takes it as a URL, in UTF-8, and would name it by its own name.
        encode_url(referrer, "the Referer in headers")
    if body is not None:
        # This takes the place of the Persona's line, as a caller's would. Having no type removes the line, while a type
        # that a caller gives for bytes still goes where Chrome puts it.
        content_type = persona.content_type(request_kind, body)
        if fetch.mode == _NO_CORS and not _is_no_cors_safelisted_content_type(content_type):
            # Blink's request-no-cors guard drops a Content-Type such as JSON's, so the body goes as the text it is,
            # with the type that Chrome gives text.
            content_type = persona.content_type(request_kind, EncodedBody(b"", BodyEncoding.TEXT))
        changes.setdefault(_CONTENT_TYPE.lower(), (_CONTENT_TYPE, content_type))
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
        body=body if body is None or isinstance(body, StreamBody) else body.content,
    )


def _with_fetch(
    defaults: RequestDefaults, kind: RequestKind, method: str, credentials: str | None, mode: str | None
) -> RequestDefaults:
    """Return what a kind sends with the credentials mode and the mode that a caller gives in place of the kind's own.

    A mode keeps its line's place, and an element in cors mode gets the Origin that Blink puts first in its map.

    Raises:
        ValueError: If `credentials` or `mode` is none of its values, or one that `kind` or `method` cannot have.
    """
    if credentials is not None and credentials not in _CREDENTIALS:
        raise ValueError(f"credentials has to be one of {', '.join(sorted(_CREDENTIALS))}, not {credentials!r}")
    if mode is not None and mode not in _MODES:
        raise ValueError(f"mode has to be one of {', '.join(sorted(_MODES))}, not {mode!r}")
    if kind in _NAVIGATIONS and (mode is not None or credentials not in {None, _INCLUDE}):
        raise ValueError(f"a request of the kind {kind.value!r} navigates, with credentials, and takes no mode")
    if kind is RequestKind.WORKER and mode not in {None, _SAME_ORIGIN}:
        raise ValueError("the script of a worker goes in mode same-origin")
    if mode == _NO_CORS and method not in _NO_CORS_METHODS:
        raise ValueError(f"Fetch refuses mode no-cors with {method}, so Chrome never sends it")
    fetch = defaults["fetch"]
    if mode is None or mode == fetch.mode:
        lines = defaults["headers"]
    else:
        lines = tuple((name, mode if name == _SEC_FETCH_MODE else value) for name, value in defaults["headers"])
        if mode == _CORS and fetch.destination != "empty" and all(name != _ORIGIN for name, _ in lines):
            lines = ((_ORIGIN, _ORIGIN_PLACEHOLDER), *lines)
    return RequestDefaults(
        headers=lines,
        priority=defaults["priority"],
        priority_incremental=defaults["priority_incremental"],
        fetch=dataclasses.replace(
            fetch, mode=mode or fetch.mode, credentials_mode=credentials or fetch.credentials_mode
        ),
    )


def _is_no_cors_safelisted_content_type(content_type: str | None) -> bool:
    """Return whether a request in mode no-cors takes this Content-Type that the library gives a body, or none.

    Upstream: services/network/public/cpp/cors/cors.cc
    """
    if content_type is None:
        return True
    return content_type.split(";", 1)[0].strip().lower() in _NO_CORS_CONTENT_TYPES


def _with_size_from_headers(body: StreamBody, changes: dict[str, tuple[str, str | None]]) -> StreamBody:
    """Return a stream with the size in a caller's Content-Length, which //net writes in its own place from that size.

    This takes the header out of `changes`.

    Raises:
        ValueError: If the value is not a number of bytes.
    """
    _, value = changes.pop(_CONTENT_LENGTH, (_CONTENT_LENGTH, None))
    if value is None:
        return body
    if not (value.isascii() and value.isdigit()):
        raise ValueError(f"the Content-Length of a stream has to be a number of bytes, not {value!r}")
    return with_declared_size(body, int(value))


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


def _changes_by_name(headers: object) -> dict[str, tuple[str, str | None]]:
    """Return a caller's (name, value) pairs by lower-case name, of which None has none.

    Raises:
        TypeError: If `headers` is not a mapping or None, a name is not text, or a value is neither text nor None. The
            message leaves out the value, which may be a secret.
        ValueError: If two names differ only in case, since neither is more right than the other.
    """
    if headers is None:
        return {}
    if not isinstance(headers, Mapping):
        raise TypeError(f"headers has to be a mapping of names to text or None, not {type(headers).__name__}")
    lines = cast(Mapping[object, object], headers)
    changes: dict[str, tuple[str, str | None]] = {}
    for name, value in lines.items():
        if not isinstance(name, str):
            raise TypeError(f"a name in headers has to be text, not {type(name).__name__}")
        if value is not None and not isinstance(value, str):
            raise TypeError(f"the value of {name!r} in headers has to be text or None, not {type(value).__name__}")
        changes[name.lower()] = (name, value)
    if len(changes) != len(lines):
        raise ValueError(f"headers holds names that differ only in case: {list(lines)}")
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
