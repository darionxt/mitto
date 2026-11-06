"""The public API: a Session, and get() as a shortcut on a default one."""

import functools
from collections.abc import Sequence
from types import TracebackType
from typing import Self, Unpack

from mitto import _native
from mitto._native import PrivateSettings, encode_url
from mitto._persona import ENGINE_RELEASE_PERSONA, load_persona
from mitto._request import RequestOptions, build_request
from mitto._response import Response, to_response
from mitto._timeout import Timeout, to_timeout

__all__ = ["Session", "get"]


class Session:
    """A long-lived client that holds one Chrome-like browsing state: its Persona and its open connections.

    One Session may run requests from several threads at once. Close it with close() or a with statement, which ends the
    requests still running, but not while another thread is starting one.

    Each method takes the keyword arguments of RequestOptions: `params`, `headers`, `kind`, `timeout`, `data`, `json` and
    `files`.
    """

    def __init__(
        self,
        *,
        persona: str = ENGINE_RELEASE_PERSONA,
        accept_language: str | None = None,
        additional_trust_anchors: Sequence[str] = (),
        timeout: float | Timeout | None = None,
        _settings: PrivateSettings | None = None,
    ) -> None:
        """Create the Session.

        Args:
            persona: The Persona to present, such as "chrome_157_windows". The default is the Engine Release's.
            accept_language: The value of the Accept-Language header in place of the Persona's. An empty string sends none.
            additional_trust_anchors: Root certificates that this Session trusts beside the Chrome Root Store. Each entry is
                the text of a PEM file that holds at least one. A self-signed server certificate can be its own root. The
                network stack refuses QUIC to a root that is not known, so a server whose chain ends at one of these is
                reached over HTTP/1.1 or HTTP/2, whatever its Alt-Svc says.
            timeout: What a request's `timeout` defaults to: a number is the total time in seconds, and a Timeout sets each
                timer. The default runs no timer, leaving what //net has of its own.
            _settings: Settings that only tests need, which are not part of the public API.

        Raises:
            ValueError: If there is no such Persona, the native core refuses `accept_language` or
                `additional_trust_anchors`, or `timeout` is not a positive number of at most 2,000,000 seconds.
            NativeLibraryError: If the native library cannot be used.
        """
        self._persona = load_persona(persona)
        self._timeout = to_timeout(timeout)
        self._native = _native.Session(
            accept_language=self._persona.accept_language if accept_language is None else accept_language,
            additional_trust_anchors=additional_trust_anchors,
            **(PrivateSettings() if _settings is None else _settings),
        )

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    def close(self) -> None:
        """Close the Session's connections. Requests still running end with a RequestError, and so do later ones."""
        self._native.close()

    def request(self, method: str, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Fetch `url` and block until the whole response has arrived, with the GIL released while it waits.

        Args:
            method: The HTTP method. Fetch's normalized methods are sent in upper case however they are given, as Chrome
                does, and any other is sent as it is written.
            url: The URL to fetch.
            **options: See RequestOptions.

        Raises:
            TypeError: If `data` is of no type that makes a body, or `json` holds a value that JSON has no form of.
            ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8, a header cannot be encoded as Latin-1,
                `kind` is not a Request Kind, two names in `headers` differ only in case or `timeout` is not a positive
                number of at most 2,000,000 seconds. Also if the arguments of a body do not go together, `json` holds a
                NaN or an infinity, or a body goes with GET or HEAD or with a Request Kind that Chrome sends none with.
            RequestError: If the request ends without a response. Its subclass says what a caller does next, and a timer
                that runs out is a RequestTimeoutError. An HTTP error status such as 404 is a response, which
                Response.raise_for_status() turns into an error. An invalid header or method ends the request with
                ERR_INVALID_ARGUMENT.
        """
        options.setdefault("timeout", self._timeout)
        built = build_request(self._persona, method, url, **options)
        return to_response(self._native.request(**built))

    def get(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a GET request, as request() does."""
        return self.request("GET", url, **options)

    def post(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a POST request, as request() does."""
        return self.request("POST", url, **options)

    def put(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a PUT request, as request() does."""
        return self.request("PUT", url, **options)

    def patch(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a PATCH request, as request() does."""
        return self.request("PATCH", url, **options)

    def delete(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a DELETE request, as request() does."""
        return self.request("DELETE", url, **options)

    def head(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send a HEAD request, as request() does."""
        return self.request("HEAD", url, **options)

    def options(self, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Send an OPTIONS request, as request() does."""
        return self.request("OPTIONS", url, **options)


@functools.cache
def default_session() -> Session:
    """Return the Session that get() uses, which stays open until the process exits.

    Threads that race here may each create one. The ones that lose stay open unused, which does no harm.
    """
    return Session()


def get(url: str, **options: Unpack[RequestOptions]) -> Response:
    """Send a GET request on a default Session, which presents the Engine Release's Persona.

    Takes the arguments of Session.get(). A navigation is the default, as it is there.

    Raises:
        ValueError: As Session.request() does.
        NativeLibraryError: If the native library cannot be used.
        RequestError: As Session.request() does.
    """
    # An invalid URL is reported before anything else, whether or not there is a native library.
    encode_url(url)
    return default_session().get(url, **options)
