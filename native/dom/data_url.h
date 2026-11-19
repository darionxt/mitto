// A data: URL's response, as //net's DataURL::BuildResponse() makes it for
// Blink's ResourceLoader, which answers an element's request of a data: URL
// in the renderer, with no request on the wire. The Page program has no
// //net, so this ports it.
//
// Upstream: net/base/data_url.cc
// Upstream: net/base/mime_util.cc

#ifndef MITTO_DOM_DATA_URL_H_
#define MITTO_DOM_DATA_URL_H_

#include <optional>
#include <string>

class GURL;

namespace mitto {

struct DataUrlResponse {
  // The MIME type's essence, in lower case, such as "image/svg+xml".
  std::string mime_type;
  std::string charset;
  // The Content-Type line's value: the MIME type with its parameters.
  std::string content_type;
  std::string body;
};

// The response of `url`, or nullopt for one that is no data: URL Chrome can
// answer, as ERR_INVALID_URL.
std::optional<DataUrlResponse> ParseDataUrl(const GURL& url);

}  // namespace mitto

#endif  // MITTO_DOM_DATA_URL_H_
