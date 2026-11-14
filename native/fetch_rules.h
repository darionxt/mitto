#ifndef MITTO_FETCH_RULES_H_
#define MITTO_FETCH_RULES_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "mitto/client_hints.h"
#include "mitto/network_quality.h"
#include "mitto/request_options.h"
#include "net/cookies/site_for_cookies.h"
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
// Upstream: content/browser/browsing_data/clear_site_data_utils.cc
// Upstream: content/browser/client_hints/client_hints.cc
// Upstream: content/browser/client_hints/critical_client_hints_throttle.cc
// Upstream: content/browser/loader/navigation_url_loader_impl.cc
// Upstream: content/browser/loader/navigation_url_loader_impl.h
// Upstream: content/browser/loader/url_loader_throttles.cc
// Upstream: content/browser/renderer_host/navigation_request.cc
// Upstream: content/public/common/referrer.cc
// Upstream: services/network/accept_ch_frame_interceptor.cc
// Upstream: services/network/cors/cors_url_loader.cc
// Upstream: services/network/url_loader.cc
// Upstream: services/network/url_loader_util.cc
// Upstream: third_party/blink/common/client_hints/client_hints.cc
// Upstream: third_party/blink/common/loader/throttling_url_loader.cc
// Upstream: third_party/blink/renderer/core/loader/frame_fetch_context.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_loader.cc
// Upstream: third_party/blink/renderer/platform/weborigin/security_policy.cc
//
// A Persona's line keeps its place in the headers, and this sets its value or
// removes it. A name in `FetchParams::fixed_headers` is the caller's, so
// nothing here touches it.
class FetchRules {
 public:
  // What a navigation goes on with, in place of the request it had.
  struct Restart {
    Restart();
    Restart(Restart&&);
    Restart& operator=(Restart&&);
    ~Restart();

    GURL url;
    net::HttpRequestHeaders headers;

    // Whether it carries on the chain of URLs of the request it replaces, or
    // starts a new one.
    bool keeps_url_chain = false;

    // Whether it is a restart for Critical-CH, which ThrottlingURLLoader makes
    // a redirect of, and not one for an ACCEPT_CH frame, which the network
    // service makes within the request.
    bool for_critical_ch = false;
  };

  // `url` is the URL the request starts at. `client_hints_store` and
  // `network_quality` are the Session's, which outlives its requests. Without a
  // network quality, rtt and downlink keep the values of their lines.
  FetchRules(FetchParams params,
             const GURL& url,
             ClientHintsStore& client_hints_store,
             const NetworkQuality* network_quality);

  FetchRules(const FetchRules&) = delete;
  FetchRules& operator=(const FetchRules&) = delete;

  ~FetchRules();

  // To call after the request has its headers and method, and before it starts.
  void ConfigureRequest(net::URLRequest& request);

  // To call on `request` when it has made a connection, with what its server
  // said in ALPS: NavigationURLLoaderImpl::OnAcceptCHFrameReceived(). Returns
  // what to go on with if the request has to start over to send more hints.
  // After too many of those, `request_restarts_exceeded()` says so instead.
  std::optional<Restart> OnConnected(const net::URLRequest& request,
                                     std::string_view accept_ch_frame);

  // To call on `request` when it has the headers of a response, also of a
  // redirect, before anything else happens to it. Returns what to go on with if
  // the request has to start over: CriticalClientHintsThrottle.
  std::optional<Restart> OnResponseHeaders(const net::URLRequest& request);

  bool request_restarts_exceeded() const {
    return accept_ch_restart_limit_ == 0;
  }

  // Whether the request has to fail before it goes where it is about to go,
  // because its mode is same-origin and that is another origin than its page's:
  // CorsURLLoader's CorsError::kDisallowedByMode. To ask after
  // ConfigureRequest() and WillFollowRedirect().
  bool IsDisallowedByMode() const {
    return fetch_cors_flag_ && params_.mode == RequestMode::kSameOrigin;
  }

  // CorsURLLoader::CheckPreflightRequired(): whether a request in cors mode
  // with this method and these lines needs a preflight where it is about to
  // go, which is another origin than its page's. To ask after
  // ConfigureRequest() or WillFollowRedirect().
  bool NeedsPreflight(std::string_view method,
                      const net::HttpRequestHeaders& headers) const;

  const FetchParams& params() const { return params_; }

  // Whether a redirect has made the request's origin "null".
  bool tainted() const { return tainted_; }

  // To call for what takes the place of `old_request`, once it has its URL,
  // method and headers, and before it starts.
  void ConfigureRestartedRequest(net::URLRequest& request,
                                 const net::URLRequest& old_request,
                                 bool keeps_url_chain);

  // To call on a redirect that the request is about to follow. This changes
  // some of the request's headers, and says what to do with others when it
  // follows.
  void WillFollowRedirect(net::URLRequest& request,
                          const net::RedirectInfo& redirect_info,
                          std::vector<std::string>& removed_headers,
                          net::HttpRequestHeaders& modified_headers);

 private:
  // Whether this is the navigation of a frame, whose parent is the page, and
  // not one of a top-level page.
  bool IsSubframeNavigation() const;
  // The origin of the request's top-level frame: the one that a Page's frame
  // names, or the initiator. Not for a navigation that the browser starts.
  const url::Origin& TopFrameOrigin() const;
  // The site for cookies of a subresource's frame, or of a frame's parent for
  // its navigation.
  net::SiteForCookies FrameSiteForCookies() const;

  // Whether Blink holds the Origin line in its HTTPHeaderMap, with the page's
  // origin as its value: FetchParameters::SetCrossOriginAccessControl() sets it
  // for an element in cors mode, such as a font or a module script. A fetch()
  // and an XMLHttpRequest, whose destination is empty, get theirs from
  // CorsURLLoader only where it needs one.
  bool OriginIsBlinksLine() const;

  // url_loader_util::SetRequestCredentials(). The decision not to send
  // credentials lasts for the rest of the request. `url` is the URL it is about
  // to go to.
  void SetRequestCredentials(net::URLRequest& request, const GURL& url) const;

  // A cross-origin request in cors or same-origin mode, once and for all:
  // `fetch_cors_flag_` in network::cors::CorsURLLoader.
  void SetCorsFlagIfNeeded(const GURL& url);

  // CorsURLLoader::ShouldIncludeOriginHeader(). A setting that blocks cookies
  // adds a reason upstream, and a Session has no such setting.
  bool ShouldIncludeOriginHeader(std::string_view method) const;

  // The value of the Origin header in CorsURLLoader::MaybeSetOriginHeader().
  std::string OriginHeaderValue() const;

  // Sets what a request starting at its URL says about its site, its partition
  // and its page, which also comes from what a navigation has become.
  void ConfigureSiteAndReferrer(net::URLRequest& request);

  // What FrameFetchContext::AddClientHintsIfNecessary() does at the start of a
  // subresource, and AddNavigationRequestClientHintsHeaders() of a navigation.
  void ConfigureClientHints(net::URLRequest& request);

  // Whether a request to `url` sends the hint of this name, from what its
  // origin has asked for. `additional_hints` counts as asked for too.
  bool ShouldAddClientHint(const GURL& url,
                           std::string_view name,
                           const EnabledClientHints& additional_hints) const;

  // The value of a navigation's hint `name` on a hop to `url`, where its line
  // in the Persona says `value`: rtt and downlink are worked out for the host
  // of `url`, and the others keep `value`.
  std::string NavigationClientHintValue(std::string_view name,
                                        const std::string& value,
                                        const GURL& url) const;

  // ParseAndPersistAcceptCHForNavigation().
  void ParseAndPersistAcceptCHForNavigation(
      const url::Origin& origin,
      const ParsedClientHintsHeaders& parsed);

  // CriticalClientHintsThrottle::MaybeRestartWithHints(). Returns whether a
  // hint that `url` should now have was not on the request at its start.
  bool MaybeRestartWithHints(const GURL& url,
                             const ParsedClientHintsHeaders& parsed);

  bool IsFixed(std::string_view name) const;

  const FetchParams params_;
  const raw_ref<ClientHintsStore> client_hints_store_;
  const raw_ptr<const NetworkQuality> network_quality_;

  // Where the request comes from. Without an Initiator, a subresource comes
  // from the page at the root of its URL's origin, and a navigation from no
  // page.
  GURL page_;
  std::optional<url::Origin> initiator_;

  // Whether a redirect has gone through another origin than the page's:
  // `tainted_` in CorsURLLoader.
  bool tainted_ = false;
  bool fetch_cors_flag_ = false;

  // Every hint that a navigation can send, in its order, with the value of its
  // line, which it adds again on each hop if the next URL should have it.
  std::vector<std::pair<std::string, std::string>> client_hints_;

  // The URL a navigation started at, and the hints it started with: what
  // CriticalClientHintsThrottle::WillStartRequest() keeps.
  GURL original_url_;
  EnabledClientHints initial_client_hints_;

  // Whether it started with a method for which Chrome makes that throttle at
  // all.
  bool has_critical_client_hints_throttle_ = false;

  // What a restart for Critical-CH sent, with the values it sent, which also go
  // to the next hop.
  std::vector<std::pair<std::string, std::string>> client_hints_of_restart_;

  // The origins that restarted this navigation for a Critical-CH, once each.
  std::set<url::Origin> restarted_origins_;

  // NavigationURLLoaderImpl::accept_ch_restart_limit_, which is
  // net::URLRequest::kMaxRedirects.
  int accept_ch_restart_limit_ = 20;
};

}  // namespace mitto

#endif  // MITTO_FETCH_RULES_H_
