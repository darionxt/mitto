// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from services/network/sec_header_helpers.cc, with the upstream names
// kept. A caller's own line is what differs: a renderer cannot give one.

#include "mitto/sec_header_helpers.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/strings/string_util.h"
#include "net/base/is_potentially_trustworthy.h"
#include "net/base/url_util.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_request.h"
#include "url/origin.h"

namespace mitto {

namespace {

constexpr std::string_view kSecFetchMode = "Sec-Fetch-Mode";
constexpr std::string_view kSecFetchSite = "Sec-Fetch-Site";
constexpr std::string_view kSecFetchUser = "Sec-Fetch-User";
constexpr std::string_view kSecFetchDest = "Sec-Fetch-Dest";
constexpr std::string_view kSecFetchStorageAccess = "Sec-Fetch-Storage-Access";

constexpr std::string_view kClientHintPrefix = "sec-ch-";
constexpr std::string_view kFetchMetadataPrefix = "sec-fetch-";

std::string_view OriginRelationString(
    std::optional<net::OriginRelation> relation) {
  if (!relation.has_value()) {
    return "none";
  }

  switch (relation.value()) {
    case net::OriginRelation::kSameOrigin:
      return "same-origin";
    case net::OriginRelation::kSameSite:
      return "same-site";
    case net::OriginRelation::kCrossSite:
      return "cross-site";
  }
}

std::string_view RequestModeToString(RequestMode mode) {
  switch (mode) {
    case RequestMode::kNavigate:
      return "navigate";
    case RequestMode::kCors:
      return "cors";
    case RequestMode::kNoCors:
      return "no-cors";
  }
}

// Walk through a URL chain and a pending redirect url to calculate their
// relationship to the origin.
net::OriginRelation GetRelationOfURLChainToOrigin(
    const std::vector<GURL>& request_chain,
    const url::Origin& origin,
    base::optional_ref<const net::RedirectInfo> pending_redirect) {
  CHECK(!request_chain.empty());

  auto origin_relation = net::OriginRelation::kSameOrigin;
  for (const GURL& target_url : request_chain) {
    origin_relation =
        std::max(origin_relation, net::GetOriginRelation(target_url, origin));
  }

  if (pending_redirect.has_value()) {
    origin_relation =
        std::max(origin_relation,
                 net::GetOriginRelation(pending_redirect->new_url, origin));
  }
  return origin_relation;
}

std::optional<net::OriginRelation> GetInitiatorRelation(
    const net::URLRequest& request,
    base::optional_ref<const net::RedirectInfo> pending_redirect) {
  // Browser-initiated requests with no initiator origin will send
  // `Sec-Fetch-Site: None`.
  if (!request.initiator().has_value()) {
    return std::nullopt;
  }
  return GetRelationOfURLChainToOrigin(
      request.url_chain(), request.initiator().value(), pending_redirect);
}

// Sets a line, unless it is a caller's.
void SetHeader(net::URLRequest& request,
               const FetchParams& params,
               std::string_view name,
               std::string_view value,
               bool overwrite) {
  if (!IsFixedHeader(params, name)) {
    request.SetExtraRequestHeaderByName(name, value, overwrite);
  }
}

// Removes a line, unless it is a caller's.
void RemoveHeader(net::URLRequest& request,
                  const FetchParams& params,
                  std::string_view name) {
  if (!IsFixedHeader(params, name)) {
    request.RemoveRequestHeaderByName(name);
  }
}

// Sec-Fetch-Site
void SetSecFetchSiteHeader(
    net::URLRequest& request,
    const FetchParams& params,
    base::optional_ref<const net::RedirectInfo> pending_redirect) {
  std::optional<net::OriginRelation> relation =
      GetInitiatorRelation(request, pending_redirect);

  SetHeader(request, params, kSecFetchSite, OriginRelationString(relation),
            /*overwrite=*/true);
}

// Sec-Fetch-Mode
void SetSecFetchModeHeader(net::URLRequest& request,
                           const FetchParams& params) {
  SetHeader(request, params, kSecFetchMode, RequestModeToString(params.mode),
            /*overwrite=*/false);
}

// Sec-Fetch-User
void SetSecFetchUserHeader(net::URLRequest& request,
                           const FetchParams& params) {
  if (params.has_user_activation) {
    SetHeader(request, params, kSecFetchUser, "?1", /*overwrite=*/true);
  } else {
    RemoveHeader(request, params, kSecFetchUser);
  }
}

// Sec-Fetch-Dest
void SetSecFetchDestHeader(net::URLRequest& request,
                           const FetchParams& params) {
  SetHeader(request, params, kSecFetchDest, params.destination,
            /*overwrite=*/true);
}

// Sec-Fetch-Storage-Access
//
// Upstream asks CookieSettingsBase::GetStorageAccessStatus(), which is nullopt
// for a URL that is not third-party. Otherwise it is "active" wherever the
// settings allow full cookie access, as a Chrome with no such settings does,
// and a Session has none.
void SetSecFetchStorageAccessHeader(
    net::URLRequest& request,
    const FetchParams& params,
    base::optional_ref<const net::RedirectInfo> pending_redirect) {
  const GURL& url =
      pending_redirect ? pending_redirect->new_url : request.url();
  const net::SiteForCookies& site_for_cookies =
      pending_redirect ? pending_redirect->new_site_for_cookies
                       : request.site_for_cookies();
  if (params.credentials_mode != CredentialsMode::kInclude ||
      site_for_cookies.IsFirstParty(url)) {
    // A credentials mode of "same-origin" or "omit" prevents including cookies
    // on the request in the first place, so we don't bother to include the
    // `Sec-Fetch-Storage-Access` header in that case. A fetch() that a page
    // makes without asking for credentials has that mode, which is why only a
    // cross-site script or image shows this header.
    RemoveHeader(request, params, kSecFetchStorageAccess);
    return;
  }
  SetHeader(request, params, kSecFetchStorageAccess, "active",
            /*overwrite=*/true);
}

}  // namespace

bool IsFixedHeader(const FetchParams& params, std::string_view name) {
  return params.fixed_headers.contains(base::ToLowerASCII(name));
}

void RemoveSecHeaders(net::URLRequest& request, const FetchParams& params) {
  const net::HttpRequestHeaders::HeaderVector request_headers =
      request.extra_request_headers().GetHeaderVector();
  for (const auto& header : request_headers) {
    if (base::StartsWith(header.key, kClientHintPrefix,
                         base::CompareCase::INSENSITIVE_ASCII) ||
        base::StartsWith(header.key, kFetchMetadataPrefix,
                         base::CompareCase::INSENSITIVE_ASCII)) {
      RemoveHeader(request, params, header.key);
    }
  }
}

void SetFetchMetadataHeaders(
    net::URLRequest& request,
    const FetchParams& params,
    base::optional_ref<const net::RedirectInfo> pending_redirect) {
  // Only append the header to potentially trustworthy URLs.
  const GURL& target_url =
      pending_redirect ? pending_redirect->new_url : request.url();
  if (!net::IsUrlPotentiallyTrustworthy(target_url)) {
    return;
  }

  SetSecFetchSiteHeader(request, params, pending_redirect);
  SetSecFetchModeHeader(request, params);
  SetSecFetchUserHeader(request, params);
  SetSecFetchDestHeader(request, params);
  SetSecFetchStorageAccessHeader(request, params, pending_redirect);
}

void MaybeRemoveSecHeaders(net::URLRequest& request,
                           const FetchParams& params,
                           const GURL& pending_redirect_url) {
  // If our redirect destination is not trusted it would not have had sec-ch-
  // or sec-fetch- prefixed headers added to it. Our previous hops may have
  // added these headers if the current url is trustworthy though so we should
  // try to remove these now.
  if (net::IsUrlPotentiallyTrustworthy(request.url()) &&
      !net::IsUrlPotentiallyTrustworthy(pending_redirect_url)) {
    RemoveSecHeaders(request, params);
  }
}

}  // namespace mitto
