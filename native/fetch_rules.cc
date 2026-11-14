#include "mitto/fetch_rules.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_util.h"
#include "mitto/cors_preflight.h"
#include "mitto/http_header_map.h"
#include "mitto/sec_header_helpers.h"
#include "net/base/is_potentially_trustworthy.h"
#include "net/base/isolation_info.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/referrer_policy.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_job.h"

namespace mitto {
namespace {

// Chrome's default strict-origin-when-cross-origin, which no Baseline's page
// changes, in the words of //net.
constexpr net::ReferrerPolicy kDefaultReferrerPolicy =
    net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN;

// The method-based part of NeedsHTTPOrigin() in NavigationRequest, which is the
// same as in CorsURLLoader::ShouldIncludeOriginHeader().
bool NeedsHTTPOrigin(std::string_view method) {
  return method != net::HttpRequestHeaders::kGetMethod &&
         method != net::HttpRequestHeaders::kHeadMethod;
}

// The lines of a subresource that Blink holds in its HTTPHeaderMap, in the
// order they go in: Origin, for an element in cors mode, and Content-Type come
// with the request, then FrameFetchContext::AddClientHintsIfNecessary() adds
// hints in this order, and PrepareRequest() adds User-Agent. The rest come
// later, from outside the map.
constexpr auto kHTTPHeaderMapInsertionOrder = std::to_array<std::string_view>({
    "origin",
    "content-type",
    "device-memory",
    "sec-ch-device-memory",
    "rtt",
    "downlink",
    "ect",
    "sec-ch-ua",
    "sec-ch-ua-mobile",
    "sec-ch-ua-arch",
    "sec-ch-ua-platform",
    "sec-ch-ua-platform-version",
    "sec-ch-ua-model",
    "sec-ch-ua-full-version",
    "sec-ch-ua-full-version-list",
    "sec-ch-ua-bitness",
    "sec-ch-ua-wow64",
    "sec-ch-ua-form-factors",
    "save-data",
    "sec-ch-prefers-reduced-transparency",
    "sec-ch-prefers-reduced-motion",
    "sec-ch-prefers-color-scheme",
    "dpr",
    "sec-ch-dpr",
    "viewport-width",
    "sec-ch-viewport-width",
    "sec-ch-viewport-height",
    "width",
    "sec-ch-width",
    "user-agent",
});

// Whether Blink holds a line of this name in its HTTPHeaderMap. Origin is
// there only where Blink sets it.
bool IsInHTTPHeaderMap(std::string_view name, bool origin_in_map) {
  if (base::EqualsCaseInsensitiveASCII(name, net::HttpRequestHeaders::kOrigin)) {
    return origin_in_map;
  }
  return std::ranges::any_of(
      kHTTPHeaderMapInsertionOrder, [name](std::string_view listed) {
        return base::EqualsCaseInsensitiveASCII(listed, name);
      });
}

// Puts what Blink holds in its HTTPHeaderMap in the order the map gives it,
// ahead of everything else, which keeps its order.
void SortAsHTTPHeaderMap(net::URLRequest& request, bool origin_in_map) {
  const net::HttpRequestHeaders& headers = request.extra_request_headers();
  std::vector<std::string_view> names;
  for (std::string_view listed : kHTTPHeaderMapInsertionOrder) {
    if (headers.HasHeader(listed) && IsInHTTPHeaderMap(listed, origin_in_map)) {
      names.push_back(listed);
    }
  }

  net::HttpRequestHeaders sorted;
  for (const std::string& name : HTTPHeaderMapOrder(names)) {
    for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
         headers.GetHeaderVector()) {
      if (base::EqualsCaseInsensitiveASCII(header.key, name)) {
        sorted.SetHeader(header.key, header.value);
      }
    }
  }
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    if (!IsInHTTPHeaderMap(header.key, origin_in_map)) {
      sorted.SetHeader(header.key, header.value);
    }
  }
  request.SetExtraRequestHeaders(sorted);
}

bool IsFrameDestination(std::string_view destination) {
  return destination == "iframe" || destination == "frame";
}

// A navigation's request before the network service sees it has no Fetch
// Metadata, which it adds after what is there. So what a restart adds goes
// ahead of those lines.
bool IsFetchMetadataHeader(std::string_view name) {
  return base::StartsWith(name, "sec-fetch-",
                          base::CompareCase::INSENSITIVE_ASCII);
}

// content::Referrer::SanitizeOriginForRequest() under the default policy.
url::Origin SanitizeOriginForRequest(const GURL& url,
                                     const url::Origin& initiator) {
  return url::Origin::Create(net::URLRequestJob::ComputeReferrerForPolicy(
      kDefaultReferrerPolicy, initiator.GetURL(), url));
}

}  // namespace

void SetUpTopLevelNavigation(net::URLRequest& request) {
  // What NavigationRequest gives a navigation in the top frame. //net then
  // moves all three to the next URL on each hop, and takes a kMainFrame request
  // as a main frame navigation for SameSite cookies. So this needs no
  // set_force_main_frame_for_same_site_cookies(), which Chrome only sets for a
  // navigation that a service worker passes on.
  const url::Origin origin = url::Origin::Create(request.url());
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromOrigin(origin);
  request.set_site_for_cookies(site_for_cookies);
  request.set_first_party_url_policy(
      net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT);
  request.set_isolation_info(
      net::IsolationInfo::Create(net::IsolationInfo::RequestType::kMainFrame,
                                 origin, origin, site_for_cookies));
}

FetchRules::Restart::Restart() = default;
FetchRules::Restart::Restart(Restart&&) = default;
FetchRules::Restart& FetchRules::Restart::operator=(Restart&&) = default;
FetchRules::Restart::~Restart() = default;

FetchRules::FetchRules(FetchParams params,
                       const GURL& url,
                       ClientHintsStore& client_hints_store,
                       const NetworkQuality* network_quality)
    : params_(std::move(params)),
      client_hints_store_(client_hints_store),
      network_quality_(network_quality),
      original_url_(url) {
  if (params_.initiator) {
    page_ = *params_.initiator;
    initiator_ = url::Origin::Create(page_);
  } else if (params_.mode != RequestMode::kNavigate || IsSubframeNavigation()) {
    initiator_ = url::Origin::Create(url);
    page_ = initiator_->GetURL();
  }
}

bool FetchRules::IsSubframeNavigation() const {
  return params_.mode == RequestMode::kNavigate &&
         IsFrameDestination(params_.destination);
}

bool FetchRules::OriginIsBlinksLine() const {
  return params_.mode == RequestMode::kCors && params_.destination != "empty";
}

FetchRules::~FetchRules() = default;

void FetchRules::ConfigureSiteAndReferrer(net::URLRequest& request) {
  const GURL& url = request.url();

  if (IsSubframeNavigation()) {
    // What NavigationRequest gives a frame's navigation:
    // RenderFrameHostImpl::ComputeIsolationInfoForNavigation(), from the
    // parent up, which here is a top-level page. The site for cookies stays
    // the parent's on each hop, and //net moves only the frame's origin, so a
    // frame of another site gets no SameSite cookie but None.
    // Upstream: content/browser/renderer_host/render_frame_host_impl.cc
    const net::SiteForCookies site_for_cookies =
        net::SiteForCookies::FromOrigin(*initiator_);
    request.set_site_for_cookies(site_for_cookies);
    request.set_isolation_info(net::IsolationInfo::Create(
        net::IsolationInfo::RequestType::kSubFrame, *initiator_,
        url::Origin::Create(url), site_for_cookies));
  } else if (params_.mode == RequestMode::kNavigate) {
    SetUpTopLevelNavigation(request);
  } else {
    // A page counts as a top-level one, since nothing says it has a parent. A
    // subresource has its page's site for cookies and for partitioning,
    // whatever its own URL and wherever a redirect takes it.
    const net::SiteForCookies site_for_cookies =
        net::SiteForCookies::FromOrigin(*initiator_);
    request.set_site_for_cookies(site_for_cookies);
    request.set_isolation_info(
        net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther,
                                   *initiator_, *initiator_, site_for_cookies));
  }
  request.set_initiator(initiator_);
  SetRequestCredentials(request, url);

  if (!IsFixed(net::HttpRequestHeaders::kReferer)) {
    // Blink's SecurityPolicy::GenerateReferrer(), and //net from there on.
    request.set_referrer_policy(kDefaultReferrerPolicy);
    request.SetReferrer(net::URLRequestJob::ComputeReferrerForPolicy(
                            kDefaultReferrerPolicy, page_, url)
                            .spec());
  }
}

void FetchRules::ConfigureRequest(net::URLRequest& request) {
  const GURL& url = request.url();
  ConfigureSiteAndReferrer(request);
  // content::CreateContentBrowserURLLoaderThrottles(), for the outermost main
  // frame only.
  has_critical_client_hints_throttle_ =
      params_.mode == RequestMode::kNavigate && !IsSubframeNavigation() &&
      net::HttpUtil::IsMethodSafe(request.method());

  // Whether the request leaves its page's origin counts whatever a caller
  // does to Origin: it decides the CORS rules and what mode same-origin
  // refuses.
  if (params_.mode != RequestMode::kNavigate) {
    SetCorsFlagIfNeeded(url);
  }
  if (!IsFixed(net::HttpRequestHeaders::kOrigin)) {
    std::optional<std::string> origin;
    if (params_.mode == RequestMode::kNavigate) {
      // AddAdditionalRequestHeaders() in NavigationRequest. A navigation that
      // the browser started has an opaque origin.
      if (NeedsHTTPOrigin(request.method())) {
        origin = initiator_
                     ? SanitizeOriginForRequest(url, *initiator_).Serialize()
                     : url::Origin().Serialize();
      }
    } else {
      // CorsURLLoader::MaybeSetOriginHeader() writes over Blink's line where
      // it needs one, and leaves it alone elsewhere.
      if (ShouldIncludeOriginHeader(request.method()) || OriginIsBlinksLine()) {
        origin = OriginHeaderValue();
      }
    }
    if (origin) {
      request.SetExtraRequestHeaderByName(net::HttpRequestHeaders::kOrigin,
                                          *origin, /*overwrite=*/true);
    } else {
      request.RemoveRequestHeaderByName(net::HttpRequestHeaders::kOrigin);
    }
  }

  ConfigureClientHints(request);

  // SetFetchMetadataHeaders() adds nothing for a URL that is not potentially
  // trustworthy. Here a Persona's lines are already there, to hold their
  // places.
  if (!net::IsUrlPotentiallyTrustworthy(url)) {
    RemoveSecHeaders(request, params_);
  }
  SetFetchMetadataHeaders(request, params_,
                          /*pending_redirect=*/std::nullopt);
}

void FetchRules::ConfigureClientHints(net::URLRequest& request) {
  const GURL& url = request.url();
  const EnabledClientHints no_additional_hints;

  // A Persona's lines are all there, to hold their places, so this removes
  // those that this request should not have.
  bool added_a_hint_that_is_not_sent_by_default = false;
  const net::HttpRequestHeaders::HeaderVector headers =
      request.extra_request_headers().GetHeaderVector();
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header : headers) {
    if (!IsClientHintHeader(header.key) || IsFixed(header.key)) {
      continue;
    }
    if (params_.mode == RequestMode::kNavigate) {
      client_hints_.emplace_back(header.key, header.value);
    }
    if (!ShouldAddClientHint(url, header.key, no_additional_hints)) {
      request.RemoveRequestHeaderByName(header.key);
      continue;
    }
    if (params_.mode == RequestMode::kNavigate) {
      initial_client_hints_.insert(base::ToLowerASCII(header.key));
      request.SetExtraRequestHeaderByName(
          header.key, NavigationClientHintValue(header.key, header.value, url),
          /*overwrite=*/true);
      continue;
    }
    if (!IsClientHintSentByDefault(header.key)) {
      added_a_hint_that_is_not_sent_by_default = true;
    }
    // A subresource's page has a renderer process of its own site.
    if (network_quality_ && IsNetworkQualityHint(header.key)) {
      request.SetExtraRequestHeaderByName(
          header.key,
          GetSubresourceNetworkQualityHint(*network_quality_, header.key,
                                           net::SchemefulSite(*initiator_),
                                           url),
          /*overwrite=*/true);
    }
  }

  // With no hint added, the Persona's order is what the map gives, and leaving
  // it alone keeps a caller's lines where they are.
  if (added_a_hint_that_is_not_sent_by_default) {
    SortAsHTTPHeaderMap(request, OriginIsBlinksLine());
  }
}

bool FetchRules::ShouldAddClientHint(
    const GURL& url,
    std::string_view name,
    const EnabledClientHints& additional_hints) const {
  const bool sent_by_default = IsClientHintSentByDefault(name);
  if (params_.mode == RequestMode::kNavigate && !IsSubframeNavigation()) {
    // ShouldAddClientHints() and ClientHintsExtendedData, where a navigation's
    // page is the top frame that is about to commit.
    const url::Origin origin = url::Origin::Create(url);
    return net::IsOriginPotentiallyTrustworthy(origin) &&
           (sent_by_default || client_hints_store_->IsEnabled(origin, name) ||
            additional_hints.contains(base::ToLowerASCII(name)));
  }
  // FrameFetchContext::ShouldSendClientHint(), and for a frame's navigation
  // ClientHintsExtendedData, whose main frame is the page. The page's origin
  // decides, and with no permissions policy of its own the page only lets its
  // own origin have anything but what goes by default.
  return net::IsUrlPotentiallyTrustworthy(url) &&
         (sent_by_default ||
          (initiator_->IsSameOriginWith(url) &&
           client_hints_store_->IsEnabled(*initiator_, name)));
}

std::string FetchRules::NavigationClientHintValue(std::string_view name,
                                                  const std::string& value,
                                                  const GURL& url) const {
  if (!network_quality_ || !IsNetworkQualityHint(name)) {
    return value;
  }
  return GetNavigationNetworkQualityHint(*network_quality_, name, url);
}

void FetchRules::ParseAndPersistAcceptCHForNavigation(
    const url::Origin& origin,
    const ParsedClientHintsHeaders& parsed) {
  if (parsed.accept_ch) {
    client_hints_store_->PersistClientHints(origin, *parsed.accept_ch);
  }
}

bool FetchRules::MaybeRestartWithHints(const GURL& url,
                                       const ParsedClientHintsHeaders& parsed) {
  if (!has_critical_client_hints_throttle_ || !parsed.accept_ch ||
      !parsed.critical_ch) {
    return false;
  }
  const url::Origin response_origin = url::Origin::Create(url);
  if (restarted_origins_.contains(response_origin) ||
      !net::IsOriginPotentiallyTrustworthy(response_origin)) {
    return false;
  }

  // Only hints in Accept-CH count. GetCriticalHintsMissingStatus() then wants
  // one that the origin has not asked for yet.
  const bool missing =
      std::ranges::any_of(*parsed.critical_ch, [&](const std::string& hint) {
        return parsed.accept_ch->contains(hint) &&
               !IsClientHintSentByDefault(hint) &&
               !client_hints_store_->IsEnabled(response_origin, hint);
      });
  if (!missing) {
    return false;
  }

  ParseAndPersistAcceptCHForNavigation(response_origin, parsed);
  restarted_origins_.insert(response_origin);

  // If a client hint header is not in the original request, restart the
  // request.
  const EnabledClientHints no_additional_hints;
  return std::ranges::any_of(client_hints_, [&](const auto& line) {
    return ShouldAddClientHint(url, line.first, no_additional_hints) &&
           !initial_client_hints_.contains(base::ToLowerASCII(line.first));
  });
}

std::optional<FetchRules::Restart> FetchRules::OnConnected(
    const net::URLRequest& request,
    std::string_view accept_ch_frame) {
  // A frame's hints follow its page's origin, which the frame in ALPS does not
  // speak for.
  if (params_.mode != RequestMode::kNavigate || IsSubframeNavigation() ||
      accept_ch_frame.empty()) {
    return std::nullopt;
  }
  const std::optional<EnabledClientHints> frame_hints =
      ParseClientHintsHeader(std::string(accept_ch_frame));
  if (!frame_hints) {
    return std::nullopt;
  }

  // AcceptCHFrameInterceptor and then GetCriticalHintsMissingStatus(): a hint
  // that the origin has asked for already needs nothing more. The two widths
  // are only for images.
  const net::HttpRequestHeaders& headers = request.extra_request_headers();
  const url::Origin origin = url::Origin::Create(request.url());
  const bool missing =
      std::ranges::any_of(*frame_hints, [&](const std::string& hint) {
        return !headers.HasHeader(hint) && hint != "width" &&
               hint != "sec-ch-width" && !IsClientHintSentByDefault(hint) &&
               !client_hints_store_->IsEnabled(origin, hint);
      });
  if (!missing) {
    return std::nullopt;
  }

  // AddNavigationRequestClientHintsHeaders() with these as additional hints,
  // for this start only. Each line that is there already keeps its place, and
  // the others go last. Only restart if new headers are actually added.
  Restart restart;
  restart.url = request.url();
  restart.keeps_url_chain = true;
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    if (!IsFetchMetadataHeader(header.key)) {
      restart.headers.SetHeader(header.key, header.value);
    }
  }
  bool adds_a_hint = false;
  for (const auto& [name, value] : client_hints_) {
    if (ShouldAddClientHint(request.url(), name, *frame_hints) &&
        !headers.HasHeader(name)) {
      restart.headers.SetHeader(
          name, NavigationClientHintValue(name, value, request.url()));
      adds_a_hint = true;
    }
  }
  if (!adds_a_hint) {
    return std::nullopt;
  }
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    if (IsFetchMetadataHeader(header.key)) {
      restart.headers.SetHeader(header.key, header.value);
    }
  }

  // A server can present another ALPS at each handshake, which makes a loop.
  --accept_ch_restart_limit_;
  return restart;
}

std::optional<FetchRules::Restart> FetchRules::OnResponseHeaders(
    const net::URLRequest& request) {
  const net::HttpResponseHeaders* const response_headers =
      request.response_headers();
  if (!response_headers) {
    return std::nullopt;
  }
  const ParsedClientHintsHeaders parsed =
      ParseClientHintsHeaders(*response_headers);
  const url::Origin origin = url::Origin::Create(request.url());

  // SiteDataClearer: the store is only read in first-party contexts, so a
  // third-party context cannot clear it.
  if (parsed.clears_client_hints &&
      (params_.mode == RequestMode::kNavigate ||
       net::SchemefulSite(*initiator_) == net::SchemefulSite(origin))) {
    client_hints_store_->ClearClientHints(origin);
  }
  if (params_.mode != RequestMode::kNavigate) {
    return std::nullopt;
  }

  if (!MaybeRestartWithHints(request.url(), parsed)) {
    // NavigationRequest::OnResponseStarted() commits nothing for these, and it
    // is at the commit that a final response counts.
    // Only the outermost main frame parses Accept-CH.
    const int response_code = response_headers->response_code();
    if (!IsSubframeNavigation() && response_code != net::HTTP_NO_CONTENT &&
        response_code != net::HTTP_RESET_CONTENT) {
      ParseAndPersistAcceptCHForNavigation(origin, parsed);
    }
    return std::nullopt;
  }

  // ThrottlingURLLoader::RestartWithURLResetNow() makes this a redirect to
  // where the navigation started. So NavigationRequest::OnRequestRedirected()
  // takes every hint away and adds those of that URL, ahead of the Fetch
  // Metadata that the next loader adds.
  const EnabledClientHints no_additional_hints;
  const net::HttpRequestHeaders& headers = request.extra_request_headers();
  Restart restart;
  restart.url = original_url_;
  restart.for_critical_ch = true;
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    const bool is_ours = IsClientHintHeader(header.key) && !IsFixed(header.key);
    if (!is_ours && !IsFetchMetadataHeader(header.key)) {
      restart.headers.SetHeader(header.key, header.value);
    }
  }
  client_hints_of_restart_.clear();
  for (const auto& [name, value] : client_hints_) {
    if (ShouldAddClientHint(original_url_, name, no_additional_hints)) {
      std::string sent = NavigationClientHintValue(name, value, original_url_);
      restart.headers.SetHeader(name, sent);
      client_hints_of_restart_.emplace_back(name, std::move(sent));
    }
  }
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    if (IsFetchMetadataHeader(header.key)) {
      restart.headers.SetHeader(header.key, header.value);
    }
  }
  return restart;
}

void FetchRules::ConfigureRestartedRequest(net::URLRequest& request,
                                           const net::URLRequest& old_request,
                                           bool keeps_url_chain) {
  ConfigureSiteAndReferrer(request);
  // ThrottlingURLLoader and NavigationURLLoaderImpl keep the Referer that the
  // navigation has come to, so one that has shrunk stays that way.
  if (!IsFixed(net::HttpRequestHeaders::kReferer)) {
    request.SetReferrer(old_request.referrer());
  }
  // A restart for Critical-CH has only the chain that the loader started with,
  // so Sec-Fetch-Site knows nothing of where the navigation went before it.
  if (keeps_url_chain) {
    request.SetURLChain(old_request.url_chain());
  }
  // As at a start. The lines came from a hop that may have been trustworthy.
  if (!net::IsUrlPotentiallyTrustworthy(request.url())) {
    RemoveSecHeaders(request, params_);
  }
  SetFetchMetadataHeaders(request, params_,
                          /*pending_redirect=*/std::nullopt);
}

void FetchRules::WillFollowRedirect(net::URLRequest& request,
                                    const net::RedirectInfo& redirect_info,
                                    std::vector<std::string>& removed_headers,
                                    net::HttpRequestHeaders& modified_headers) {
  const GURL& new_url = redirect_info.new_url;

  // The order of CorsURLLoader::OnReceiveRedirect() and FollowRedirect().
  if (initiator_ && !url::IsSameOriginWith(new_url, request.url()) &&
      !initiator_->IsSameOriginWith(request.url())) {
    tainted_ = true;
  }
  if (params_.mode == RequestMode::kCors ||
      params_.mode == RequestMode::kSameOrigin) {
    SetCorsFlagIfNeeded(new_url);
    if (ShouldIncludeOriginHeader(redirect_info.new_method) &&
        !IsFixed(net::HttpRequestHeaders::kOrigin)) {
      modified_headers.SetHeader(net::HttpRequestHeaders::kOrigin,
                                 OriginHeaderValue());
    }
  }

  // NavigationRequest::OnRequestRedirected() in the browser process: every
  // client hint goes, and those that the next URL should have come back through
  // `modified_headers`, which //net merges last, so they land after the rest.
  // rtt and downlink are worked out again, for the next URL's host.
  const EnabledClientHints no_additional_hints;
  if (params_.mode == RequestMode::kNavigate) {
    // ThrottlingURLLoader::FollowRedirect() goes on from a restart with what
    // that said still in `headers_update_params_`, which it clears only after
    // the next real redirect. So on that hop those hints come first, and go
    // whether or not the next URL's origin has asked for them. Real Chrome
    // shows this. What the redirect says merges over them, so a hint that
    // both have keeps the place of the restart's and takes the value of the
    // redirect's.
    if (net::IsUrlPotentiallyTrustworthy(new_url)) {
      for (const auto& [name, value] : client_hints_of_restart_) {
        modified_headers.SetHeader(name, value);
      }
    }
    client_hints_of_restart_.clear();
    for (const auto& [name, value] : client_hints_) {
      removed_headers.push_back(name);
      if (ShouldAddClientHint(new_url, name, no_additional_hints)) {
        modified_headers.SetHeader(
            name, NavigationClientHintValue(name, value, new_url));
      }
    }
  } else {
    // A subresource's hints come from Blink at its start only, with rtt and
    // downlink for the host it started at. blink::FindClientHintsToRemove()
    // in ResourceLoader::WillFollowRedirect() takes away what the page's
    // permissions policy does not allow the next URL, which by default leaves
    // its own origin everything, and any other what goes by default.
    // MaybeRemoveSecHeaders() below takes those at a URL that is not
    // potentially trustworthy.
    for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
         request.extra_request_headers().GetHeaderVector()) {
      if (IsClientHintHeader(header.key) && !IsFixed(header.key) &&
          !IsClientHintSentByDefault(header.key) &&
          !initiator_->IsSameOriginWith(new_url)) {
        removed_headers.push_back(header.key);
      }
    }
  }

  // URLLoader::FollowRedirect() and what comes before it.
  SetRequestCredentials(request, new_url);
  MaybeRemoveSecHeaders(request, params_, new_url);
  SetFetchMetadataHeaders(request, params_, redirect_info);
}

void FetchRules::SetRequestCredentials(net::URLRequest& request,
                                       const GURL& url) const {
  if (params_.credentials_mode == CredentialsMode::kOmit ||
      (params_.credentials_mode == CredentialsMode::kSameOrigin &&
       initiator_ && !initiator_->IsSameOriginWith(url))) {
    request.set_disallow_credentials();
  }
}

bool FetchRules::NeedsPreflight(std::string_view method,
                                const net::HttpRequestHeaders& headers) const {
  return params_.mode == RequestMode::kCors && fetch_cors_flag_ &&
         mitto::NeedsPreflight(method, headers);
}

void FetchRules::SetCorsFlagIfNeeded(const GURL& url) {
  if ((params_.mode == RequestMode::kCors ||
       params_.mode == RequestMode::kSameOrigin) &&
      !initiator_->IsSameOriginWith(url)) {
    fetch_cors_flag_ = true;
  }
}

bool FetchRules::ShouldIncludeOriginHeader(std::string_view method) const {
  return fetch_cors_flag_ || NeedsHTTPOrigin(method);
}

std::string FetchRules::OriginHeaderValue() const {
  return tainted_ ? url::Origin().Serialize() : initiator_->Serialize();
}

bool FetchRules::IsFixed(std::string_view name) const {
  return IsFixedHeader(params_, name);
}

}  // namespace mitto
