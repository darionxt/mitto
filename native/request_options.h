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
#include "net/url_request/referrer_policy.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

// The values of network::mojom::RequestMode that a Request Kind has.
enum class RequestMode {
  kNavigate,
  kSameOrigin,
  kCors,
  kNoCors,
};

// The values of network::mojom::CredentialsMode.
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

  // The referrer policy of the request, or strict-origin-when-cross-origin,
  // Chrome's default, which no Baseline's page changes. A Page's request can
  // name another.
  std::optional<net::ReferrerPolicy> referrer_policy;

  // The URL that Referer starts from, of the initiator's origin, in place of
  // the initiator itself, as fetch()'s `referrer` asks. Always valid.
  std::optional<GURL> referrer_source;

  // For a request of a Page's frame, or a frame's navigation, whose top-level
  // frame is not the initiator: the top-level frame's origin, which
  // partitions the request and gives it its site for cookies, as Chrome's
  // browser works it out from the frame tree. nullopt counts the initiator,
  // or for a frame's navigation its parent, as the top-level one, as for a
  // caller's request.
  std::optional<url::Origin> top_frame_origin;
  // Whether a frame from the request's own one, or for a frame's navigation
  // from its parent, up to the top-level one is not schemefully same-site
  // with the top-level one, which leaves the request no site for cookies.
  bool cross_site_ancestor = false;
  // Whether a frame on that way up is not same-origin with the top-level
  // one. The default permissions policy then lets the request have no client
  // hint beyond those that go by default.
  bool cross_origin_ancestor = false;
  // A dedicated worker's request, which sends no client hints.
  bool from_worker = false;

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
