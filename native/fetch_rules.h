#ifndef MITTO_FETCH_RULES_H_
#define MITTO_FETCH_RULES_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mitto/request_options.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
struct RedirectInfo;
class URLRequest;
}  // namespace net

namespace mitto {

// Says that `request` is a navigation that the browser started at its URL, with
// nothing about its headers changing: what a request made without FetchParams
// gets.
void SetUpTopLevelNavigation(net::URLRequest& request);

// What real Chrome does to a request because of the page it comes from, at its
// start and on each hop. In Chrome this is spread over Blink, the browser
// process and the network service, which the source export leaves out, so it is
// ported here with the upstream names kept. What each part comes from is on the
// part.
//
// A Persona's line keeps its place in the headers, and this sets its value or
// removes it. A name in `FetchParams::fixed_headers` is the caller's, so
// nothing here touches it.
class FetchRules {
 public:
  // `url` is the URL the request starts at.
  FetchRules(FetchParams params, const GURL& url);

  FetchRules(const FetchRules&) = delete;
  FetchRules& operator=(const FetchRules&) = delete;

  ~FetchRules();

  // To call after the request has its headers and method, and before it starts.
  void ConfigureRequest(net::URLRequest& request);

  // To call on a redirect that the request is about to follow. This changes
  // some of the request's headers, and says what to do with others when it
  // follows.
  void WillFollowRedirect(net::URLRequest& request,
                          const net::RedirectInfo& redirect_info,
                          std::vector<std::string>& removed_headers,
                          net::HttpRequestHeaders& modified_headers);

 private:
  // A cross-origin request in cors mode, once and for all: `fetch_cors_flag_`
  // in network::cors::CorsURLLoader.
  void SetCorsFlagIfNeeded(const GURL& url);

  // CorsURLLoader::ShouldIncludeOriginHeader(). A setting that blocks cookies
  // adds a reason upstream, and a Session has no such setting.
  bool ShouldIncludeOriginHeader(std::string_view method) const;

  // The value of the Origin header in CorsURLLoader::MaybeSetOriginHeader().
  std::string OriginHeaderValue() const;

  bool IsFixed(std::string_view name) const;

  const FetchParams params_;

  // Where the request comes from. Without an Initiator, a subresource comes
  // from the page at the root of its URL's origin, and a navigation from no
  // page.
  GURL page_;
  std::optional<url::Origin> initiator_;

  // Whether a redirect has gone through another origin than the page's:
  // `tainted_` in CorsURLLoader.
  bool tainted_ = false;
  bool fetch_cors_flag_ = false;

  // The lines that a navigation adds again on each hop, in their order.
  std::vector<std::pair<std::string, std::string>> client_hints_;
};

}  // namespace mitto

#endif  // MITTO_FETCH_RULES_H_
