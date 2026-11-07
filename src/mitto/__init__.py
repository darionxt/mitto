"""An HTTP client whose traffic matches real Chrome, built on Chromium's network stack."""

from mitto._api import Session, get
from mitto._async_api import AsyncSession
from mitto._cookies import AsyncCookies, Cookie, CookiePartitionKey, Cookies
from mitto._errors import (
    CertificateError,
    ConnectionFailedError,
    MittoError,
    NameResolutionError,
    NativeLibraryError,
    ProtocolError,
    ProxyError,
    RequestError,
    RequestTimeoutError,
    TLSError,
    TooManyRedirectsError,
)
from mitto._persona import RequestKind
from mitto._response import AsyncResponseStream, Headers, HTTPStatusError, Redirect, Response, ResponseStream
from mitto._timeout import Timeout

__all__ = [
    "AsyncCookies",
    "AsyncResponseStream",
    "AsyncSession",
    "CertificateError",
    "ConnectionFailedError",
    "Cookie",
    "CookiePartitionKey",
    "Cookies",
    "HTTPStatusError",
    "Headers",
    "MittoError",
    "NameResolutionError",
    "NativeLibraryError",
    "ProtocolError",
    "ProxyError",
    "Redirect",
    "RequestError",
    "RequestKind",
    "RequestTimeoutError",
    "Response",
    "ResponseStream",
    "Session",
    "TLSError",
    "Timeout",
    "TooManyRedirectsError",
    "get",
]
