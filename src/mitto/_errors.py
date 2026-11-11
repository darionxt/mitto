"""What mitto raises, sorted by what a caller does next."""

from collections.abc import Mapping

__all__ = [
    "ERROR_TYPES_BY_NAME",
    "ERROR_TYPES_BY_PREFIX",
    "CertificateError",
    "ConnectionFailedError",
    "MittoError",
    "NameResolutionError",
    "NativeLibraryError",
    "PageEndedError",
    "PageError",
    "PageTimeoutError",
    "ProtocolError",
    "ProxyError",
    "RequestError",
    "RequestTimeoutError",
    "ScriptError",
    "TLSError",
    "TooManyRedirectsError",
    "request_error",
]


class MittoError(Exception):
    """The base of everything mitto raises."""


class NativeLibraryError(MittoError):
    """The native library is missing, cannot be loaded or was built for another ABI version."""


class RequestError(MittoError):
    """A request ended without a response.

    A subclass says what went wrong in terms of what a caller does next. This class itself stands for anything else, such
    as a request that was cancelled or a URL that is not valid.

    Attributes:
        url: The URL that was requested.
        net_error: The negative Chromium net error code, such as -105.
        method: The HTTP method of the request.
        name: The name of the net error in the Engine Release, such as "ERR_NAME_NOT_RESOLVED".
    """

    def __init__(self, url: str, net_error: int, method: str, name: str) -> None:
        # Exception pickles its args, so they have to be what __init__ takes. This is not super().__init__() because
        # OSError, which RequestTimeoutError also is, would take them for an errno and a message.
        Exception.__init__(self, url, net_error, method, name)
        self.url = url
        self.net_error = net_error
        self.method = method
        self.name = name

    def __str__(self) -> str:
        return f"{self.method} {self.url} failed with {self.name} ({self.net_error})"


class NameResolutionError(RequestError):
    """The host's name did not resolve. Check the name and the DNS settings."""


class ConnectionFailedError(RequestError):
    """No connection could be made, or the one in use broke. This is often worth another try."""


class TLSError(RequestError):
    """TLS failed for a reason other than the server's certificate. Another try rarely helps."""


class CertificateError(TLSError):
    """The server's certificate was refused, so its identity is not proven. Another try does not help."""


class ProxyError(RequestError):
    """The proxy failed or refused the request, which says nothing about the server behind it. Check the proxy."""


class ProtocolError(RequestError):
    """The server broke the rules of HTTP, or a response ended early. Another try may help."""


class TooManyRedirectsError(RequestError):
    """A chain of redirects went past //net's limit, which often means a loop. Another try rarely helps."""


class RequestTimeoutError(RequestError, TimeoutError):
    """A timer of the request ran out, or one that //net has of its own. This is often worth another try, or a longer one."""


class PageError(MittoError):
    """A call to a Page failed. A subclass says what a caller does next. Private until the Runtime ships.

    Attributes:
        message: What went wrong.
    """

    def __init__(self, message: str) -> None:
        super().__init__(message)
        self.message = message


class ScriptError(PageError):
    """The script threw, the promise it gave was rejected, or its result cannot be read as JSON. The Page goes on.

    The message is what a console says, such as "Uncaught TypeError: x is not a function". Fix the script.
    """


class PageEndedError(PageError):
    """The Page has ended: its process crashed, ran out of memory or was killed, or its Session closed. Open another one.

    A Page whose process breaks the rules of its messages ends too, and so does one whose time zone or locale ICU does
    not know. Each later call to the Page raises this again, with the same message.
    """


class PageTimeoutError(PageError, TimeoutError):
    """The timeout passed before the script's result arrived. Wait longer, or close the Page.

    The script may still be running: one that never ends keeps the Page busy, and later calls wait behind it. A promise
    that it gave may still settle, which nothing waits for then.

    Attributes:
        timeout: How many seconds the call waited.
    """

    def __init__(self, timeout: float) -> None:
        # Exception pickles its args, so they have to be what __init__ takes, and OSError, which this also is, would
        # take a message and more for an errno.
        message = f"the result of the script did not arrive within {timeout} seconds"
        Exception.__init__(self, timeout)
        self.message = message
        self.timeout = timeout

    def __str__(self) -> str:
        return self.message


# What a net error's name alone does not say. Everything here comes before ERROR_TYPES_BY_PREFIX.
ERROR_TYPES_BY_NAME: Mapping[str, type[RequestError]] = {
    name: error_type
    for error_type, names in (
        (RequestTimeoutError, ("ERR_CONNECTION_TIMED_OUT", "ERR_DNS_TIMED_OUT", "ERR_TIMED_OUT")),
        (
            NameResolutionError,
            (
                "ERR_HOST_RESOLVER_QUEUE_TOO_LARGE",
                "ERR_ICANN_NAME_COLLISION",
                "ERR_NAME_NOT_RESOLVED",
                "ERR_NAME_RESOLUTION_FAILED",
            ),
        ),
        (ConnectionFailedError, ("ERR_EMPTY_RESPONSE", "ERR_INTERNET_DISCONNECTED", "ERR_SOCKET_NOT_CONNECTED")),
        (
            TLSError,
            (
                "ERR_ALPN_NEGOTIATION_FAILED",
                "ERR_BAD_SSL_CLIENT_AUTH_CERT",
                "ERR_CLIENT_AUTH_CERT_TYPE_UNSUPPORTED",
                "ERR_ECH_NOT_NEGOTIATED",
                "ERR_INVALID_ECH_CONFIG_LIST",
                "ERR_QUIC_HANDSHAKE_FAILED",
                "ERR_STRICT_ECH_REQUIRED",
                "ERR_TLS13_DOWNGRADE_DETECTED",
            ),
        ),
        (
            CertificateError,
            (
                "ERR_CERTIFICATE_TRANSPARENCY_REQUIRED",
                "ERR_ECH_FALLBACK_CERTIFICATE_INVALID",
                "ERR_INSECURE_RESPONSE",
                "ERR_QUIC_CERT_ROOT_NOT_KNOWN",
                "ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN",
                "ERR_SSL_SERVER_CERT_BAD_FORMAT",
                "ERR_SSL_SERVER_CERT_CHANGED",
            ),
        ),
        (
            ProxyError,
            (
                "ERR_MANDATORY_PROXY_CONFIGURATION_FAILED",
                "ERR_NO_SUPPORTED_PROXIES",
                "ERR_TUNNEL_CONNECTION_FAILED",
                "ERR_UNABLE_TO_REUSE_CONNECTION_FOR_PROXY_AUTH",
                "ERR_UNEXPECTED_PROXY_AUTH",
            ),
        ),
        (
            ProtocolError,
            (
                "ERR_HTTP_1_1_REQUIRED",
                "ERR_INCOMPLETE_CHUNKED_ENCODING",
                "ERR_INCOMPLETE_HTTP2_HEADERS",
                "ERR_INVALID_CHUNKED_ENCODING",
                "ERR_INVALID_HTTP_RESPONSE",
                "ERR_INVALID_REDIRECT",
                "ERR_INVALID_RESPONSE",
                "ERR_UNSAFE_REDIRECT",
                "ERR_ZSTD_WINDOW_SIZE_TOO_BIG",
            ),
        ),
        (TooManyRedirectsError, ("ERR_TOO_MANY_REDIRECTS",)),
    )
    for name in names
}

# The families of net error, so that one that a later Engine Release adds lands in the right place.
ERROR_TYPES_BY_PREFIX: Mapping[str, type[RequestError]] = {
    "ERR_ADDRESS_": ConnectionFailedError,
    "ERR_CERT_": CertificateError,
    "ERR_CONNECTION_": ConnectionFailedError,
    "ERR_CONTENT_": ProtocolError,
    "ERR_DNS_": NameResolutionError,
    "ERR_HTTP2_": ProtocolError,
    "ERR_NETWORK_": ConnectionFailedError,
    "ERR_PAC_": ProxyError,
    "ERR_PROXY_": ProxyError,
    "ERR_QUIC_": ProtocolError,
    "ERR_RESPONSE_HEADERS_": ProtocolError,
    "ERR_SOCKS_": ProxyError,
    "ERR_SSL_": TLSError,
}


def request_error(url: str, net_error: int, method: str, name: str) -> RequestError:
    """Return the error for a net error, of the type that says what a caller does next.

    Args:
        url: The URL that was requested.
        net_error: The negative Chromium net error code.
        method: The HTTP method of the request.
        name: The name of the net error, which decides the type.
    """
    return _error_type(name)(url, net_error, method, name)


def _error_type(name: str) -> type[RequestError]:
    if name in ERROR_TYPES_BY_NAME:
        return ERROR_TYPES_BY_NAME[name]
    for prefix, error_type in ERROR_TYPES_BY_PREFIX.items():
        if name.startswith(prefix):
            return error_type
    return RequestError
