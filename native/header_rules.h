// The Fetch Standard's rules for a request's method and header lines, as
// Chrome applies them. A Page applies them to what its scripts give fetch(),
// and its Session again to what the Page asks for, since a Page runs a site's
// scripts and may send anything.

#ifndef MITTO_HEADER_RULES_H_
#define MITTO_HEADER_RULES_H_

#include <string>
#include <string_view>

namespace mitto {

// A token: a header's name, or a method.
// Upstream: net/http/http_util.cc
bool IsHttpToken(std::string_view text);

// Whether `value` can be a header line's value: no NUL, CR or LF.
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
bool IsValidHeaderValue(std::string_view value);

// `value` without HTTP whitespace at either end.
// Upstream: third_party/blink/renderer/platform/loader/fetch/fetch_utils.cc
std::string_view NormalizeHeaderValue(std::string_view value);

// Whether a script may not set the line `name`: `value`, which the browser
// sets, as net::HttpUtil::IsSafeHeader() says for Chrome.
// Upstream: net/http/http_util.cc
bool IsForbiddenRequestHeader(std::string_view name, std::string_view value);

// Set-Cookie and Set-Cookie2, which a script never reads.
// Upstream: third_party/blink/renderer/platform/loader/fetch/fetch_utils.cc
bool IsForbiddenResponseHeaderName(std::string_view name);

// CONNECT, TRACE and TRACK, which a script never sends, in any case.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsForbiddenMethod(std::string_view method);

// GET, HEAD and POST, which need no CORS preflight.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsCorsSafelistedMethod(std::string_view method);

// Accept, Accept-Language, Content-Language and Content-Type, in any case: the
// only names that a request of mode no-cors takes from a script.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsNoCorsSafelistedHeaderName(std::string_view name);

// Range, which a script may delete from a request of mode no-cors.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsPrivilegedNoCorsHeaderName(std::string_view name);

// Whether a request of mode no-cors takes the line `name`: `value` from a
// script: one of those names, with a value that IsCorsSafelistedHeader()
// passes.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsNoCorsSafelistedHeader(std::string_view name, std::string_view value);

// Whether `media_type` is a CORS-safelisted type, whatever its length:
// application/x-www-form-urlencoded, multipart/form-data or text/plain, with
// no byte that a CORS-safelisted line may not hold, as a beacon's Blob type is
// checked.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsCorsSafelistedContentType(std::string_view media_type);

// The response header names that a script reads of a cross-origin response
// beside those that Access-Control-Expose-Headers names.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsCorsSafelistedResponseHeaderName(std::string_view name);

// The values of each header line named `name`, joined by commas, as Blink's
// ResourceResponse::HttpHeaderField() joins a header that comes more than
// once. `headers` holds a line feed after each "name: value".
std::string HeaderField(std::string_view headers, std::string_view name);

// Whether a response's Cache-Control value, `raw_value`, has the directive
// no-store, as Blink's ParseCacheControlDirectives() finds it.
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
bool CacheControlContainsNoStore(std::string_view raw_value);

// Whether one of the lines of `headers` has a Location to go to, which
// net::HttpResponseHeaders::IsRedirect() wants of a redirect.
bool HasLocation(std::string_view headers);

// Whether a request's line of `name` makes Blink's ResourceRequest
// conditional, as If-Match, If-Modified-Since, If-None-Match, If-Range and
// If-Unmodified-Since do.
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_request.cc
bool IsConditionalHeader(std::string_view name);

}  // namespace mitto

#endif  // MITTO_HEADER_RULES_H_
