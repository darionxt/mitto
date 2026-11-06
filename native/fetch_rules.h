#ifndef MITTO_FETCH_RULES_H_
#define MITTO_FETCH_RULES_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ref.h"
#include "mitto/client_hints.h"
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
  // What a navigation goes on with, in place of the request it had.
  struct Restart {
    Restart();
    Restart(Restart&&);
    Restart& operator=(Restart&&);
    ~Restart();

    GURL url;
    net::HttpRequestHeaders headers;

    // Whether it goes back to the method and the body the navigation started
    // with, or takes those that it has come to.
    bool from_the_start = false;
  };

  // `url` is the URL the request starts at. `client_hints_store` is the
  // Session's, which outlives its requests.
  FetchRules(FetchParams params,
             const GURL& url,
             ClientHintsStore& client_hints_store);

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

  // To call for what takes the place of `old_request`, once it has its URL,
  // method and headers, and before it starts.
  void ConfigureRestartedRequest(net::URLRequest& request,
                                 const net::URLRequest& old_request);

  // To call on a redirect that the request is about to follow. This changes
  // some of the request's headers, and says what to do with others when it
  // follows.
  void WillFollowRedirect(net::URLRequest& request,
                          const net::RedirectInfo& redirect_info,
                          std::vector<std::string>& removed_headers,
                          net::HttpRequestHeaders& modified_headers);

 private:
  // url_loader_util::SetRequestCredentials(). The decision not to send
  // credentials lasts for the rest of the request. `url` is the URL it is about
  // to go to.
  void SetRequestCredentials(net::URLRequest& request, const GURL& url) const;

  // A cross-origin request in cors mode, once and for all: `fetch_cors_flag_`
  // in network::cors::CorsURLLoader.
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

  // Where the request comes from. Without an Initiator, a subresource comes
  // from the page at the root of its URL's origin, and a navigation from no
  // page.
  GURL page_;
  std::optional<url::Origin> initiator_;

  // Whether a redirect has gone through another origin than the page's:
  // `tainted_` in CorsURLLoader.
  bool tainted_ = false;
  bool fetch_cors_flag_ = false;

  // Every hint that a navigation can send, in its order, which it adds again on
  // each hop if the next URL should have it.
  std::vector<std::pair<std::string, std::string>> client_hints_;

  // The URL a navigation started at, and the hints it started with: what
  // CriticalClientHintsThrottle::WillStartRequest() keeps.
  GURL original_url_;
  EnabledClientHints initial_client_hints_;

  // The origins that restarted this navigation for a Critical-CH, once each.
  std::set<url::Origin> restarted_origins_;

  // NavigationURLLoaderImpl::accept_ch_restart_limit_, which is
  // net::URLRequest::kMaxRedirects.
  int accept_ch_restart_limit_ = 20;
};

}  // namespace mitto

#endif  // MITTO_FETCH_RULES_H_
