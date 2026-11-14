#ifndef MITTO_REQUEST_OPTIONS_H_
#define MITTO_REQUEST_OPTIONS_H_

#include <stdint.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "net/base/request_priority.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"

namespace mitto {

// The values of network::mojom::RequestMode that a Request Kind has.
enum class RequestMode {
  kNavigate,
  kSameOrigin,
  kCors,
  kNoCors,
};

// The values of network::mojom::CredentialsMode that a Request Kind has.
enum class CredentialsMode {
  kOmit,
  kSameOrigin,
  kInclude,
};

// What a page's request says of itself, in the terms of Chrome's
// network::ResourceRequest. See mitto_request_start().
struct FetchParams {
  FetchParams();
  FetchParams(const FetchParams&);
  FetchParams& operator=(const FetchParams&);
  FetchParams(FetchParams&&);
  FetchParams& operator=(FetchParams&&);
  ~FetchParams();

  RequestMode mode = RequestMode::kNavigate;
  // Always a token, because the Sec-Fetch-Dest header holds it.
  std::string destination;
  CredentialsMode credentials_mode = CredentialsMode::kInclude;
  bool has_user_activation = false;

  // The URL of the page the request comes from. Always valid.
  std::optional<GURL> initiator;

  // The names of the headers that are the caller's, in lower case.
  std::set<std::string> fixed_headers;
};

// What a request is started with, besides its URL and headers.
struct RequestOptions {
  RequestOptions();
  RequestOptions(const RequestOptions&);
  RequestOptions& operator=(const RequestOptions&);
  RequestOptions(RequestOptions&&);
  RequestOptions& operator=(RequestOptions&&);
  ~RequestOptions();

  std::string method = net::HttpRequestHeaders::kGetMethod;
  net::RequestPriority priority = net::DEFAULT_PRIORITY;
  bool priority_incremental = net::kDefaultPriorityIncremental;

  // The URL to send as the Referer header on each hop. Empty leaves it to
  // `fetch`, or sends none if there is none.
  GURL referrer;

  // Without this, a request gets the headers it is given and no more, and
  // counts as a navigation that the browser started.
  std::optional<FetchParams> fetch;

  // False makes a redirect the response, as mitto_request_start() describes.
  bool follow_redirects = true;

  // A Page's reload, which validates what the cache holds: //net's
  // LOAD_VALIDATE_CACHE. No option of the C API's sets it.
  bool validate_cache = false;

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
