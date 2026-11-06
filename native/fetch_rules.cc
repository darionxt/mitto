#include "mitto/fetch_rules.h"

#include <string_view>
#include <utility>

#include "mitto/sec_header_helpers.h"
#include "net/base/is_potentially_trustworthy.h"
#include "net/base/isolation_info.h"
#include "net/cookies/site_for_cookies.h"
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

FetchRules::FetchRules(FetchParams params, const GURL& url)
    : params_(std::move(params)) {
  if (params_.initiator) {
    page_ = *params_.initiator;
    initiator_ = url::Origin::Create(page_);
  } else if (params_.mode != RequestMode::kNavigate) {
    initiator_ = url::Origin::Create(url);
    page_ = initiator_->GetURL();
  }
}

FetchRules::~FetchRules() = default;

void FetchRules::ConfigureRequest(net::URLRequest& request) {
  const GURL& url = request.url();

  if (params_.mode == RequestMode::kNavigate) {
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
  SetRequestCredentials(request, params_, initiator_, url);

  if (!IsFixed(net::HttpRequestHeaders::kReferer)) {
    // Blink's SecurityPolicy::GenerateReferrer(), and //net from there on.
    request.set_referrer_policy(kDefaultReferrerPolicy);
    request.SetReferrer(net::URLRequestJob::ComputeReferrerForPolicy(
                            kDefaultReferrerPolicy, page_, url)
                            .spec());
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
      SetCorsFlagIfNeeded(url);
      if (ShouldIncludeOriginHeader(request.method())) {
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

  if (params_.mode == RequestMode::kNavigate) {
    for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
         request.extra_request_headers().GetHeaderVector()) {
      if (IsClientHintHeader(header.key) && !IsFixed(header.key)) {
        client_hints_.emplace_back(header.key, header.value);
      }
    }
  }

  // Blink and the browser process add a client hint only for a potentially
  // trustworthy URL, and SetFetchMetadataHeaders() adds nothing for another.
  // Here a Persona's lines are already there, to hold their places.
  if (!net::IsUrlPotentiallyTrustworthy(url)) {
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
  if (params_.mode == RequestMode::kCors) {
    SetCorsFlagIfNeeded(new_url);
    if (ShouldIncludeOriginHeader(redirect_info.new_method) &&
        !IsFixed(net::HttpRequestHeaders::kOrigin)) {
      modified_headers.SetHeader(net::HttpRequestHeaders::kOrigin,
                                 OriginHeaderValue());
    }
  }

  // NavigationRequest::OnRequestRedirected() in the browser process: every
  // client hint goes, and those that the next URL should have come back through
  // `modified_headers`, which //net merges last, so they land after the rest. A
  // subresource's come from Blink at its start only, so they stay where they
  // are until a URL that is not potentially trustworthy, and then stay gone.
  if (params_.mode == RequestMode::kNavigate) {
    const bool add_client_hints =
        net::IsOriginPotentiallyTrustworthy(url::Origin::Create(new_url));
    for (const auto& [name, value] : client_hints_) {
      removed_headers.push_back(name);
      if (add_client_hints) {
        modified_headers.SetHeader(name, value);
      }
    }
  }

  // URLLoader::FollowRedirect() and what comes before it.
  SetRequestCredentials(request, params_, initiator_, new_url);
  MaybeRemoveSecHeaders(request, params_, new_url);
  SetFetchMetadataHeaders(request, params_, redirect_info);
}

void FetchRules::SetCorsFlagIfNeeded(const GURL& url) {
  if (params_.mode == RequestMode::kCors &&
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
