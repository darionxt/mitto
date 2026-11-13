// The MIME type and charset of a response, from its Content-Type, as //net
// works them out for the renderer, which takes the charset as the encoding the
// response names. The Page does not link //net, so this ports what it needs.
//
// Upstream: net/http/http_response_headers.cc
// Upstream: net/http/http_util.cc
// Upstream: net/base/mime_util.cc

#ifndef MITTO_ENCODING_CONTENT_TYPE_H_
#define MITTO_ENCODING_CONTENT_TYPE_H_

#include <string>
#include <string_view>

namespace mitto {

struct MimeTypeAndCharset {
  // In lower case, without parameters, or empty.
  std::string mime_type;
  // In lower case, as the response gives it, or empty.
  std::string charset;
};

// //net's HttpResponseHeaders::GetMimeTypeAndCharset(), from `content_type`,
// the values of each Content-Type line joined by ", ", as
// GetNormalizedHeader() joins them. Each value goes through
// HttpUtil::ParseContentType() in turn: a later media type replaces an earlier
// one, and keeps its charset only if it names the same type.
MimeTypeAndCharset GetMimeTypeAndCharset(std::string_view content_type);

}  // namespace mitto

#endif  // MITTO_ENCODING_CONTENT_TYPE_H_
