#ifndef MITTO_REQUEST_OPTIONS_H_
#define MITTO_REQUEST_OPTIONS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "net/base/request_priority.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"

namespace mitto {

// What a request is started with, besides its URL and headers.
struct RequestOptions {
  RequestOptions();
  RequestOptions(RequestOptions&&);
  RequestOptions& operator=(RequestOptions&&);
  ~RequestOptions();

  std::string method = net::HttpRequestHeaders::kGetMethod;
  net::RequestPriority priority = net::DEFAULT_PRIORITY;
  bool priority_incremental = net::kDefaultPriorityIncremental;

  // The URL to send as the Referer header. Empty sends none.
  GURL referrer;

  // False makes a redirect the response, as mitto_request_start() describes.
  bool follow_redirects = true;

  // A stream body follows, of `stream_body_size` bytes if that is known, as
  // mitto_request_start() describes.
  bool stream_body = false;
  std::optional<uint64_t> stream_body_size;

  // What each timer allows, as mitto_request_start() describes. nullopt runs no
  // timer. A value is always positive.
  std::optional<base::TimeDelta> connect_timeout;
  std::optional<base::TimeDelta> read_timeout;
  std::optional<base::TimeDelta> total_timeout;
};

// Reads the JSON format described at mitto_request_start() in mitto.h. Returns
// nullopt if `json` is not valid in that format. `json` comes from outside the
// library, so anything may be in it.
std::optional<RequestOptions> ParseRequestOptions(std::string_view json);

}  // namespace mitto

#endif  // MITTO_REQUEST_OPTIONS_H_
