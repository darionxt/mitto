"""The public API: a Session, and get() as a shortcut on a default one."""

import functools
import os
from collections.abc import Generator, Sequence
from contextlib import contextmanager
from pathlib import Path
from types import TracebackType
from typing import Self, Unpack

from mitto import _native
from mitto._cookies import Cookies
from mitto._native import MEMORY_CACHE, PrivateSettings, encode_url, iter_body, wait_for_response
from mitto._persona import ENGINE_RELEASE_PERSONA, load_persona
from mitto._request import RequestOptions, build_request
from mitto._response import Response, ResponseStream, to_response, to_response_stream
from mitto._timeout import Timeout, to_timeout

__all__ = ["Session", "get"]


class Session:
    """A long-lived client that holds one Chrome-like browsing state: its Persona, cookies, cache and open connections.

    One Session may run requests from several threads at once. Close it with close() or a with statement, which ends the
    requests still running, but not while another thread is starting one or using its cookies.

    Each method takes the keyword arguments of RequestOptions: `params`, `headers`, `kind`, `initiator`, `timeout`,
    `follow_redirects`, `data`, `json` and `files`.

    Attributes:
        cookies: The cookies, which //net sets from responses and sends with requests.
    """

    def __init__(
        self,
        *,
        persona: str = ENGINE_RELEASE_PERSONA,
        accept_language: str | None = None,
        additional_trust_anchors: Sequence[str] = (),
        timeout: float | Timeout | None = None,
        cookie_file: str | os.PathLike[str] | None = None,
        proxy: str | None = None,
        cache: str | os.PathLike[str] | None = None,
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
            cookie_file: A file to keep the cookies in. They load from it now, if it exists, and close() saves them to it,
                as Cookies.save() does. A crash loses what came after the last save, which a caller can make more often
                with `session.cookies.save(path)`. The default keeps them only until the Session closes.
            proxy: The proxy for every request, as a URL: "http://user:password@host:port", or the same with the scheme
                https or socks5. A username and password are percent-encoded, and answer the proxy when it asks, as they do
                in Chrome once a person has typed them. A SOCKS5 proxy cannot take them yet. As in Chrome, a request to
                localhost, a *.localhost name or a loopback address goes around the proxy, and a proxy that fails is an
                error and not a reason to go directly. The default reaches each server directly, whatever the system's
                proxy settings say.
            cache: Where to keep responses, which //net then answers from and revalidates as Chrome's HTTP cache does, split
                by the top-level site of the page a request comes from. A directory, made if it does not exist, is what
                Chrome has and keeps them for later Sessions too. //net only touches its subdirectory Cache_Data, and one
                Session at a time can have the directory, in any process. "memory" keeps them until the Session closes, as
                Chrome's Incognito mode does, and a server can tell the two apart by the stream a 304 cancels over HTTP/2.
                Write "./memory" for a directory of that name. The default keeps no responses, where a real Chrome always
                has a cache, so a server sees each request.
            _settings: Settings that only tests need, which are not part of the public API.

        Raises:
            ValueError: If there is no such Persona, the native core refuses `accept_language` or
                `additional_trust_anchors`, `timeout` is not a positive number of at most 2,000,000 seconds,
                `cookie_file` does not hold cookies that this Session can use, `proxy` cannot be used or `cache` is an empty
                string. The message leaves out `proxy`, which may hold a password.
            OSError: If `cookie_file` exists and cannot be read, a cache cannot be kept in the directory of `cache`, or
                another Session has that directory open.
            NativeLibraryError: If the native library cannot be used.
        """
        self._persona = load_persona(persona)
        self._timeout = to_timeout(timeout)
        # Before anything that needs closing exists, because this can raise. The working directory may change before the
        # Session closes.
        self._cookie_file = None if cookie_file is None else Path(cookie_file).resolve()
        self._native = _native.Session(
            accept_language=self._persona.accept_language if accept_language is None else accept_language,
            user_agent=self._persona.user_agent,
            additional_trust_anchors=additional_trust_anchors,
            proxy=proxy,
            http_cache=_http_cache_setting(cache),
            **(PrivateSettings() if _settings is None else _settings),
        )
        self.cookies = Cookies(self._native)
        if self._cookie_file is not None:
            self._load_cookie_file(self._cookie_file)

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    def close(self) -> None:
        """Save the cookies to `cookie_file`, if it has one, and close the Session's connections and any cache on disk.

        Requests still running end with a RequestError, and so do later ones. A response that arrives during this call
        may not reach the file. The connections close even if the save fails, and a second call does nothing. Another
        Session can open the same `cache` once this has returned.

        Raises:
            OSError: If `cookie_file` cannot be written.
        """
        cookie_file, self._cookie_file = self._cookie_file, None
        try:
            if cookie_file is not None:
                self.cookies.save(cookie_file)
        finally:
            self._native.close()

    def request(self, method: str, url: str, **options: Unpack[RequestOptions]) -> Response:
        """Fetch `url` and block until the whole response has arrived, with the GIL released while it waits.

        Args:
            method: The HTTP method. Fetch's normalized methods are sent in upper case however they are given, as Chrome
                does, and any other is sent as it is written.
            url: The URL to fetch.
            **options: See RequestOptions.

        Raises:
            TypeError: If `data` is of no type that makes a body or a stream gives something other than bytes, or `json`
                holds a value that JSON has no form of.
            ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8, a header cannot be encoded as Latin-1,
                `kind` is not a Request Kind, two names in `headers` differ only in case or `timeout` is not a positive
                number of at most 2,000,000 seconds. Also if the arguments of a body do not go together, `json` holds a
                NaN or an infinity, a body goes with GET or HEAD or with a Request Kind that Chrome sends none with, or a
                stream does not keep to its `Content-Length`.
            RequestError: If the request ends without a response. Its subclass says what a caller does next, and a timer
                that runs out is a RequestTimeoutError. A proxy that asks for credentials this Session does not have, or
                refuses those it has, is a ProxyError with ERR_PROXY_AUTH_REQUESTED. An HTTP error status such as 404 is a
                response, which Response.raise_for_status() turns into an error. An invalid header or method, or an
                `initiator` that is not a valid URL, ends the request with ERR_INVALID_ARGUMENT.
            Exception: Whatever a stream's file or iterable raises, which ends the request.
        """
        options.setdefault("timeout", self._timeout)
        built = build_request(self._persona, method, url, **options)
        return to_response(self._native.request(**built))

    @contextmanager
    def stream(self, method: str, url: str, **options: Unpack[RequestOptions]) -> Generator[ResponseStream]:
        """Fetch `url` in a with statement, and read the response's body as it arrives.

        Entering the with statement blocks until the response's headers have arrived, with any stream body gone by then,
        and the GIL released while it waits. Leaving it ends the request if that is still running.

        Args:
            method: The HTTP method, as for request().
            url: The URL to fetch.
            **options: See RequestOptions.

        Raises:
            TypeError: As for request().
            ValueError: As for request().
            RequestError: As for request(), on entering the with statement. An end before the end of the body is raised by
                what reads it.
            Exception: Whatever a stream's file or iterable raises, which ends the request.
        """
        options.setdefault("timeout", self._timeout)
        built = build_request(self._persona, method, url, **options)
        with self._native.start(**built) as request:
            started, history = wait_for_response(request)
            yield to_response_stream(started, history, iter_body(request))

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

    def _load_cookie_file(self, path: Path) -> None:
        """Load what `path` holds, where a file that does not exist yet holds nothing. Close the Session if that fails."""
        try:
            self.cookies.load(path)
        except FileNotFoundError:
            pass
        except BaseException:
            self._native.close()
            raise


def _http_cache_setting(cache: str | os.PathLike[str] | None) -> str | None:
    """Return the native Session's setting for what Session() takes as `cache`.

    Raises:
        ValueError: If `cache` is an empty string, which would otherwise mean the working directory.
    """
    if cache is None:
        return None
    if cache == "":
        raise ValueError("cache is an empty string, which names no directory")
    if cache == MEMORY_CACHE:
        return MEMORY_CACHE
    # The working directory may change before the Session closes, and the native core only takes an absolute path.
    return str(Path(cache).resolve())


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
