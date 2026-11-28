#ifndef MITTO_REQUEST_OPTIONS_H_
#define MITTO_REQUEST_OPTIONS_H_

#include <stdint.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "base/unguessable_token.h"
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

// Whose order a request's headers go in, once a page's script, or a caller in
// its place, has set some.
enum class HeaderOrder {
  // The order they are given in.
  kGiven,
  // That of Blink's HTTPHeaderMap for a fetch() or an XMLHttpRequest.
  kFetch,
  kXhr,
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
  // For mode kCors: network::mojom::RequestMode::kCorsWithForcedPreflight,
  // which a fetch() whose body is a stream has, so that a request to another
  // origin always goes after a CORS preflight.
  bool force_preflight = false;
  // Always a token, because the Sec-Fetch-Dest header holds it.
  std::string destination;
  CredentialsMode credentials_mode = CredentialsMode::kInclude;
  bool has_user_activation = false;

  // The URL of the page the request comes from. Always valid.
  std::optional<GURL> initiator;
  // The opaque origin that the request comes from, as a dedicated worker of a
  // data: URL's is, in place of `initiator`: its Origin is "null", it comes
  // from another site than any URL's, and it names no page to refer from.
  // It is the context's own, the same for each of its requests, so that they
  // share their partition and their preflights, as in Chrome.
  // `top_frame_origin` then gives the top-level frame that partitions it.
  std::optional<url::Origin> opaque_initiator;

  // The referrer policy of the request, or strict-origin-when-cross-origin,
  // Chrome's default, which no Baseline's page changes. A Page's request can
  // name another.
  std::optional<net::ReferrerPolicy> referrer_policy;

  // The URL that Referer starts from, of the initiator's origin, in place of
  // the initiator itself, as fetch()'s `referrer` asks, or, for a traversal
  // of a Page's session history, the Referer that the entry keeps, of any
  // origin, as Chrome's browser keeps it, which the referrer policy applies
  // to again for the request's URL. Always valid.
  std::optional<GURL> referrer_source;

  // A request of a context whose outgoing referrer is none, as an
  // about:blank document's or a worker's of a blob: or data: URL is, which
  // sends no Referer under any referrer policy, as Blink's
  // SecurityPolicy::GenerateReferrer() gives none for an empty one.
  bool no_referrer_source = false;

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
  // For a request of a credentialless document, or a frame's navigation that
  // makes one, the nonce of the top-level document's credentialless frames,
  // which its IsolationInfo takes, as RenderFrameHostImpl::ComputeNonce()
  // gives it: //net then keys its cookies, connections and cache entries by it.
  std::optional<base::UnguessableToken> nonce;
  // For a request of a worker of a data: URL with a nonce: its creator's
  // frame's origin, which its IsolationInfo takes for the frame's, where its
  // opaque origin stays the request's initiator.
  std::optional<url::Origin> isolation_frame_origin;
  // A dedicated worker's request, which sends no client hints.
  bool from_worker = false;
  // Whether the document or worker that asks has the embedder policy
  // "credentialless", which sends and keeps no credentials on a request of
  // mode no-cors to another origin.
  bool credentialless_client = false;

  // The names of the headers that are the caller's, in lower case.
  std::set<std::string> fixed_headers;

  // The same names, each once, in the order that the script or the caller set
  // them.
  std::vector<std::string> script_headers;
  // Whether a page's script sent them, so that Blink's map holds each in any
  // mode, as a request of cache.keys(), of mode no-cors, keeps lines that need
  // a preflight. A caller's line of mode no-cors that needs one is no script's.
  bool script_headers_of_a_page = false;
  HeaderOrder header_order = HeaderOrder::kGiven;
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

  // //net's load flags for the cache of a Page's request: LOAD_VALIDATE_CACHE
  // for a reload, and a fetch()'s cache mode's, as Blink's
  // GetLoadFlagsForWebUrlRequest() gives them, so that //net adds the lines
  // that go with them. No option of the C API's sets them.
  int cache_load_flags = 0;

  // A Page's traversal of its session history to another document, which
  // takes what the cache holds without validating it: //net's
  // LOAD_SKIP_CACHE_VALIDATION. No option of the C API's sets it.
  bool skip_cache_validation = false;

  // A Page's synchronous request, which goes past the limits of connections,
  // since the Page's thread, which waits for it, may be what holds them:
  // //net's LOAD_IGNORE_LIMITS, at `priority` HIGHEST. No option of the C API's
  // sets it.
  bool ignore_limits = false;

  // A stream body follows, of `stream_body_size` bytes if that is known, as
  // mitto_request_start() describes.
  bool stream_body = false;
  // The client hears how the upload of the body goes.
  bool report_upload_progress = false;
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
