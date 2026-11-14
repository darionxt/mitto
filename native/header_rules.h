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

// Whether a cross-origin request may carry the line `name`: `value` that a
// script set without a CORS preflight. Only the Fetch Standard's safelisted
// names count; a range is not counted as simple.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsCorsSafelistedRequestHeader(std::string_view name,
                                   std::string_view value);

// The response header names that a script reads of a cross-origin response
// beside those that Access-Control-Expose-Headers names.
// Upstream: services/network/public/cpp/cors/cors.cc
bool IsCorsSafelistedResponseHeaderName(std::string_view name);

// The values of each header line named `name`, joined by commas, as Blink's
// ResourceResponse::HttpHeaderField() joins a header that comes more than
// once. `headers` holds a line feed after each "name: value".
std::string HeaderField(std::string_view headers, std::string_view name);

// Blink's ResourceResponse::HttpContentType(): the MIME type of the last
// media type in Content-Type that has one, in lower case, without parameters.
std::string HttpContentType(std::string_view headers);

}  // namespace mitto

#endif  // MITTO_HEADER_RULES_H_
