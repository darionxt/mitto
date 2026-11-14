#include "mitto/cors_preflight.h"

#include <stdint.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "base/containers/fixed_flat_set.h"
#include "base/memory/ptr_util.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "mitto/header_rules.h"
#include "mitto/sec_header_helpers.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/cert/cert_status_flags.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_request_context.h"

namespace mitto {
namespace {

// PreflightResult: how long a result lasts without Access-Control-Max-Age, and
// at most.
constexpr base::TimeDelta kDefaultTimeout = base::Seconds(5);
constexpr base::TimeDelta kMaxTimeout = base::Hours(2);

// PreflightCache.
constexpr size_t kMaxKeyLength = 1024;
constexpr size_t kMaxEntriesPerTopFrameSite = 32;
constexpr size_t kPurgeUnitPerTopFrameSite = 4;
constexpr size_t kMaxTopFrameSites = 128;

constexpr std::string_view kAsterisk = "*";
constexpr std::string_view kLowerCaseTrue = "true";
constexpr std::string_view kAccessControlAllowOrigin =
    "Access-Control-Allow-Origin";
constexpr std::string_view kAccessControlAllowCredentials =
    "Access-Control-Allow-Credentials";
constexpr std::string_view kAccessControlAllowMethods =
    "Access-Control-Allow-Methods";
constexpr std::string_view kAccessControlAllowHeaders =
    "Access-Control-Allow-Headers";
constexpr std::string_view kAccessControlMaxAge = "Access-Control-Max-Age";
constexpr std::string_view kAccessControlRequestMethod =
    "Access-Control-Request-Method";
constexpr std::string_view kAccessControlRequestHeaders =
    "Access-Control-Request-Headers";
constexpr std::string_view kDefaultAcceptHeaderValue = "*/*";

bool IsSimilarToDoubleABNF(std::string_view value) {
  if (value.empty() || !base::IsAsciiDigit(value.front())) {
    return false;
  }
  bool period_found = false;
  bool digit_found_after_period = false;
  for (const char c : value) {
    if (base::IsAsciiDigit(c)) {
      digit_found_after_period |= period_found;
      continue;
    }
    if (c != '.' || period_found) {
      return false;
    }
    period_found = true;
  }
  return !period_found || digit_found_after_period;
}

bool IsSimilarToIntABNF(std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, base::IsAsciiDigit<char>);
}

// network::IsSuccessfulStatus(): an ok status.
bool IsSuccessfulStatus(int status) {
  return status >= 200 && status < 300;
}

std::optional<std::string> HeaderValue(const net::HttpResponseHeaders& headers,
                                       std::string_view name) {
  return headers.GetNormalizedHeader(name);
}

// ParseAccessControlAllowList(). An empty set for a value that is not a list
// of tokens.
bool ParseAccessControlAllowList(const std::optional<std::string>& value,
                                 base::flat_set<std::string>& set,
                                 bool insert_in_lower_case) {
  if (!value) {
    return true;
  }
  net::HttpUtil::ValuesIterator it(*value, /*delimiter=*/',',
                                   /*ignore_empty_values=*/true);
  while (it.GetNext()) {
    const std::string_view item = it.value();
    if (!net::HttpUtil::IsToken(item)) {
      set.clear();
      return false;
    }
    set.insert(insert_in_lower_case ? base::ToLowerASCII(item)
                                    : std::string(item));
  }
  return true;
}

base::TimeDelta ParseAccessControlMaxAge(
    const std::optional<std::string>& max_age) {
  int64_t seconds = 0;
  if (!max_age || !base::StringToInt64(*max_age, &seconds)) {
    return kDefaultTimeout;
  }
  if (seconds < 0) {
    return base::TimeDelta();
  }
  if (seconds >= kMaxTimeout.InSeconds()) {
    return kMaxTimeout;
  }
  return base::Seconds(seconds);
}

// CreateAccessControlRequestHeadersHeader(): sorted, lower case, without
// duplicates or spaces.
std::string CreateAccessControlRequestHeadersHeader(
    const net::HttpRequestHeaders& headers) {
  std::vector<std::string> names =
      CorsUnsafeNotForbiddenRequestHeaderNames(headers);
  std::ranges::sort(names);
  const auto [first, last] = std::ranges::unique(names);
  names.erase(first, last);
  return base::JoinString(names, ",");
}

}  // namespace

bool IsCorsSafelistedHeader(std::string_view name, std::string_view value) {
  if (value.size() > 128) {
    return false;
  }
  static constexpr auto kSafeNames = base::MakeFixedFlatSet<std::string_view>({
      "accept",
      "accept-language",
      "content-language",
      "content-type",
      "range",
      "device-memory",
      "dpr",
      "width",
      "viewport-width",
      "rtt",
      "downlink",
      "ect",
      "sec-ch-ua",
      "sec-ch-ua-arch",
      "sec-ch-ua-platform",
      "sec-ch-ua-model",
      "sec-ch-ua-mobile",
      "sec-ch-ua-full-version",
      "sec-ch-ua-platform-version",
      "sec-ch-prefers-color-scheme",
      "sec-ch-ua-bitness",
      "sec-ch-viewport-height",
      "sec-ch-device-memory",
      "sec-ch-dpr",
      "sec-ch-width",
      "sec-ch-viewport-width",
      "sec-ch-ua-full-version-list",
      "sec-ch-ua-wow64",
      "save-data",
      "sec-ch-prefers-reduced-motion",
      "sec-ch-ua-form-factors",
      "sec-ch-prefers-reduced-transparency",
  });
  const std::string lower_name = base::ToLowerASCII(name);
  if (!kSafeNames.contains(lower_name)) {
    return false;
  }
  if (IsNoCorsSafelistedHeaderName(lower_name)) {
    return IsNoCorsSafelistedHeader(lower_name, value);
  }
  if (lower_name == "range") {
    const std::optional<net::HttpByteRange> range =
        net::HttpUtil::ParseFetchSingleRange(value,
                                             /*allow_whitespace=*/false);
    return range && !range->IsSuffixByteRange() && range->IsValid() &&
           range->first_byte_position() !=
               std::numeric_limits<int64_t>::max() &&
           range->last_byte_position() != std::numeric_limits<int64_t>::max();
  }
  if (lower_name == "device-memory" || lower_name == "dpr" ||
      lower_name == "downlink") {
    return IsSimilarToDoubleABNF(value);
  }
  if (lower_name == "width" || lower_name == "viewport-width" ||
      lower_name == "rtt") {
    return IsSimilarToIntABNF(value);
  }
  if (lower_name == "ect") {
    return value == "slow-2g" || value == "2g" || value == "3g" ||
           value == "4g";
  }
  if (lower_name == "save-data") {
    return base::ToLowerASCII(value) == "on";
  }
  return true;
}

std::vector<std::string> CorsUnsafeNotForbiddenRequestHeaderNames(
    const net::HttpRequestHeaders& headers) {
  constexpr size_t kSafeListValueSizeMax = 1024;
  std::vector<std::string> header_names;
  std::vector<std::string> potentially_unsafe_names;
  size_t safe_list_value_size = 0;
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    if (!net::HttpUtil::IsSafeHeader(header.key, header.value)) {
      continue;
    }
    std::string name = base::ToLowerASCII(header.key);
    if (!IsCorsSafelistedHeader(name, header.value)) {
      header_names.push_back(std::move(name));
    } else {
      potentially_unsafe_names.push_back(std::move(name));
      safe_list_value_size += header.value.size();
    }
  }
  if (safe_list_value_size > kSafeListValueSizeMax) {
    std::ranges::move(potentially_unsafe_names,
                      std::back_inserter(header_names));
  }
  return header_names;
}

bool NeedsPreflight(std::string_view method,
                    const net::HttpRequestHeaders& headers) {
  return !IsCorsSafelistedMethod(method) ||
         !CorsUnsafeNotForbiddenRequestHeaderNames(headers).empty();
}

bool CheckAccess(const GURL& response_url,
                 const net::HttpResponseHeaders& headers,
                 CredentialsMode credentials_mode,
                 const url::Origin& origin) {
  const std::optional<std::string> allow_origin =
      HeaderValue(headers, kAccessControlAllowOrigin);
  if (allow_origin == kAsterisk) {
    if (credentials_mode != CredentialsMode::kInclude) {
      return true;
    }
    // A wildcard with credentials is refused over HTTP and HTTPS.
    if (response_url.SchemeIsHTTPOrHTTPS()) {
      return false;
    }
  } else if (!allow_origin || *allow_origin != origin.Serialize()) {
    // Missing, more than one value, "null" for an origin that is not opaque,
    // no origin at all, or another origin.
    return false;
  }
  if (credentials_mode == CredentialsMode::kInclude) {
    // Case-sensitive, as the Fetch Standard says.
    return HeaderValue(headers, kAccessControlAllowCredentials) ==
           kLowerCaseTrue;
  }
  return true;
}

std::unique_ptr<PreflightResult> PreflightResult::Create(
    CredentialsMode credentials_mode,
    const net::HttpResponseHeaders& headers) {
  std::unique_ptr<PreflightResult> result =
      base::WrapUnique(new PreflightResult(credentials_mode));
  if (!ParseAccessControlAllowList(
          HeaderValue(headers, kAccessControlAllowMethods), result->methods_,
          /*insert_in_lower_case=*/false) ||
      !ParseAccessControlAllowList(
          HeaderValue(headers, kAccessControlAllowHeaders), result->headers_,
          /*insert_in_lower_case=*/true)) {
    return nullptr;
  }
  result->absolute_expiry_time_ =
      base::TimeTicks::Now() +
      ParseAccessControlMaxAge(HeaderValue(headers, kAccessControlMaxAge));
  return result;
}

PreflightResult::PreflightResult(CredentialsMode credentials_mode)
    : credentials_(credentials_mode == CredentialsMode::kInclude) {}

PreflightResult::~PreflightResult() = default;

bool PreflightResult::EnsureAllowedCrossOriginMethod(
    std::string_view method) const {
  // A NetworkContext's `acam_preflight_spec_conformant` is true by default:
  // the method as it is, with no upper-casing.
  if (methods_.contains(std::string(method)) || IsCorsSafelistedMethod(method)) {
    return true;
  }
  return !credentials_ && methods_.contains(std::string(kAsterisk));
}

bool PreflightResult::EnsureAllowedCrossOriginHeaders(
    const net::HttpRequestHeaders& headers) const {
  // kCorsNonWildcardRequestHeadersSupport is off by default, so a wildcard
  // covers Authorization too.
  if (!credentials_ && headers_.contains(std::string(kAsterisk))) {
    return true;
  }
  return std::ranges::all_of(CorsUnsafeNotForbiddenRequestHeaderNames(headers),
                             [this](const std::string& name) {
                               return headers_.contains(name);
                             });
}

bool PreflightResult::EnsureAllowedRequest(
    CredentialsMode credentials_mode,
    std::string_view method,
    const net::HttpRequestHeaders& headers) const {
  if (!credentials_ && credentials_mode == CredentialsMode::kInclude) {
    return false;
  }
  return EnsureAllowedCrossOriginMethod(method) &&
         EnsureAllowedCrossOriginHeaders(headers);
}

bool PreflightResult::IsExpired() const {
  return absolute_expiry_time_ <= base::TimeTicks::Now();
}

PreflightCache::PreflightCache() : cache_(kMaxTopFrameSites) {}

PreflightCache::~PreflightCache() = default;

void PreflightCache::AppendEntry(
    const url::Origin& origin,
    const GURL& url,
    const net::NetworkIsolationKey& network_isolation_key,
    std::unique_ptr<PreflightResult> result) {
  std::string url_spec = url.spec();
  if (url_spec.length() >= kMaxKeyLength) {
    return;
  }
  const std::optional<net::SchemefulSite>& top_frame_site =
      network_isolation_key.GetTopFrameSite();
  auto cache_it = cache_.Get(top_frame_site);
  if (cache_it == cache_.end()) {
    cache_it = cache_.Put(top_frame_site, EntryMap());
  }
  EntryMap& entries = cache_it->second;
  EntryKey key(origin, std::move(url_spec), network_isolation_key);
  const auto found = entries.find(key);
  if (found != entries.end()) {
    found->second = std::move(result);
    return;
  }
  // MayPurge(): a run of entries from a random place goes.
  if (entries.size() > kMaxEntriesPerTopFrameSite - 1) {
    auto purge_begin = entries.begin();
    std::advance(purge_begin,
                 base::RandIntInclusive(
                     0, static_cast<int>(entries.size() -
                                         kPurgeUnitPerTopFrameSite)));
    auto purge_end = purge_begin;
    std::advance(purge_end, kPurgeUnitPerTopFrameSite);
    entries.erase(purge_begin, purge_end);
  }
  entries.emplace(std::move(key), std::move(result));
}

bool PreflightCache::CheckIfRequestCanSkipPreflight(
    const url::Origin& origin,
    const GURL& url,
    const net::NetworkIsolationKey& network_isolation_key,
    CredentialsMode credentials_mode,
    std::string_view method,
    const net::HttpRequestHeaders& headers) {
  const std::optional<net::SchemefulSite>& top_frame_site =
      network_isolation_key.GetTopFrameSite();
  auto cache_it = cache_.Peek(top_frame_site);
  if (cache_it == cache_.end()) {
    return false;
  }
  EntryMap& entries = cache_it->second;
  const auto found =
      entries.find(EntryKey(origin, url.spec(), network_isolation_key));
  if (found == entries.end()) {
    return false;
  }
  if (!found->second->IsExpired() &&
      found->second->EnsureAllowedRequest(credentials_mode, method, headers)) {
    cache_.Get(top_frame_site);
    return true;
  }
  entries.erase(found);
  if (entries.empty()) {
    cache_.Erase(cache_it);
  }
  return false;
}

PreflightRequest::PreflightRequest() = default;
PreflightRequest::PreflightRequest(const PreflightRequest&) = default;
PreflightRequest& PreflightRequest::operator=(const PreflightRequest&) =
    default;
PreflightRequest::~PreflightRequest() = default;

bool CanSkipPreflight(PreflightCache& cache, const PreflightRequest& request) {
  return !request.tainted &&
         cache.CheckIfRequestCanSkipPreflight(
             request.initiator, request.url,
             request.isolation_info.network_isolation_key(),
             request.credentials_mode, request.method, request.headers);
}

PreflightLoader::PreflightLoader(
    net::URLRequestContext& context,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    const PreflightRequest& actual,
    PreflightCache& cache,
    std::optional<net::AuthCredentials> proxy_credentials,
    base::RepeatingClosure on_connected,
    Callback callback)
    : cache_(cache),
      actual_(actual),
      proxy_credentials_(std::move(proxy_credentials)),
      on_connected_(std::move(on_connected)),
      callback_(std::move(callback)) {
  // CreatePreflightRequest(), steps 1 to 5 of the CORS-preflight fetch.
  request_ = context.CreateRequest(actual_.url, actual_.priority, this,
                                   traffic_annotation,
                                   net::handles::kInvalidNetworkHandle);
  request_->set_method(net::HttpRequestHeaders::kOptionsMethod);
  request_->SetPriorityIncremental(actual_.priority_incremental);
  request_->SetLoadFlags(actual_.load_flags &
                         (net::LOAD_VALIDATE_CACHE | net::LOAD_BYPASS_CACHE |
                          net::LOAD_DISABLE_CACHE));
  request_->set_referrer_policy(actual_.referrer_policy);
  request_->SetReferrer(actual_.referrer);
  request_->set_initiator(actual_.initiator);
  // The request's IsolationInfo as a subresource's, with no site for cookies,
  // since a preflight sends none.
  const net::IsolationInfo& isolation_info = actual_.isolation_info;
  if (!isolation_info.IsEmpty()) {
    request_->set_isolation_info(net::IsolationInfo::Create(
        net::IsolationInfo::RequestType::kOther,
        *isolation_info.top_frame_origin(), *isolation_info.frame_origin(),
        net::SiteForCookies(), isolation_info.nonce()));
  }
  request_->set_site_for_cookies(net::SiteForCookies());
  request_->set_disallow_credentials();

  net::HttpRequestHeaders headers;
  headers.SetHeader(net::HttpRequestHeaders::kAccept,
                    kDefaultAcceptHeaderValue);
  headers.SetHeader(kAccessControlRequestMethod, actual_.method);
  const std::string request_headers =
      CreateAccessControlRequestHeadersHeader(actual_.headers);
  if (!request_headers.empty()) {
    headers.SetHeader(kAccessControlRequestHeaders, request_headers);
  }
  headers.SetHeader(
      net::HttpRequestHeaders::kOrigin,
      (actual_.tainted ? url::Origin() : actual_.initiator).Serialize());
  // Copied from the actual request, as Chrome copies DevTools' override.
  if (const std::optional<std::string> user_agent =
          actual_.headers.GetHeader(net::HttpRequestHeaders::kUserAgent)) {
    headers.SetHeader(net::HttpRequestHeaders::kUserAgent, *user_agent);
  }
  // Not in the algorithm, but Chrome sends it whatever the URL.
  headers.SetHeader("Sec-Fetch-Mode", "cors");
  request_->SetExtraRequestHeaders(headers);

  // URLLoader adds the rest of the Fetch Metadata after those lines. The
  // preflight omits credentials, so it says nothing of storage access.
  FetchParams preflight_params;
  preflight_params.mode = RequestMode::kCors;
  preflight_params.destination = actual_.destination;
  preflight_params.credentials_mode = CredentialsMode::kOmit;
  SetFetchMetadataHeaders(*request_, preflight_params,
                          /*pending_redirect=*/std::nullopt);
}

PreflightLoader::~PreflightLoader() = default;

void PreflightLoader::Start() {
  request_->Start();
}

int PreflightLoader::OnConnected(net::URLRequest* request,
                                 const net::TransportInfo& info,
                                 net::CompletionOnceCallback callback) {
  on_connected_.Run();
  return net::OK;
}

void PreflightLoader::OnAuthRequired(net::URLRequest* request,
                                     const net::AuthChallengeInfo& auth_info) {
  // A server's challenge is its answer, which is not ok. A proxy's gets the
  // Session's credentials once, as the request's own hops would.
  if (!auth_info.is_proxy) {
    request->CancelAuth();
    return;
  }
  if (!proxy_credentials_ || proxy_auth_answered_) {
    request->CancelWithError(net::ERR_PROXY_AUTH_REQUESTED);
    return;
  }
  proxy_auth_answered_ = true;
  request->SetAuth(*proxy_credentials_);
}

void PreflightLoader::OnReceivedRedirect(net::URLRequest* request,
                                         const net::RedirectInfo& redirect_info,
                                         bool* defer_redirect) {
  // kPreflightDisallowedRedirect.
  *defer_redirect = true;
  Complete(net::ERR_FAILED);
}

void PreflightLoader::OnResponseStarted(net::URLRequest* request,
                                        int net_error) {
  if (net_error != net::OK) {
    Complete(net_error);
    return;
  }
  const net::HttpResponseHeaders* const headers = request->response_headers();
  if (!headers) {
    Complete(net::ERR_FAILED);
    return;
  }
  // CheckPreflightAccess(): the access check with the actual request's
  // credentials mode, then an ok status.
  const url::Origin origin =
      actual_.tainted ? url::Origin() : actual_.initiator;
  if (!CheckAccess(request->url(), *headers, actual_.credentials_mode,
                   origin) ||
      !IsSuccessfulStatus(headers->response_code())) {
    Complete(net::ERR_FAILED);
    return;
  }
  std::unique_ptr<PreflightResult> result =
      PreflightResult::Create(actual_.credentials_mode, *headers);
  if (!result || !result->EnsureAllowedCrossOriginMethod(actual_.method) ||
      !result->EnsureAllowedCrossOriginHeaders(actual_.headers)) {
    Complete(net::ERR_FAILED);
    return;
  }
  // A tainted request neither reads the cache nor adds to it, with
  // kCorsPreflightCacheKeyTaintedOrigin on by default.
  if (!(actual_.load_flags & net::LOAD_DISABLE_CACHE) && !actual_.tainted &&
      !net::IsCertStatusError(request->ssl_info().cert_status)) {
    cache_->AppendEntry(actual_.initiator, actual_.url,
                        actual_.isolation_info.network_isolation_key(),
                        std::move(result));
  }
  Complete(net::OK);
}

void PreflightLoader::OnReadCompleted(net::URLRequest* request,
                                      int bytes_read) {
  // The preflight is done once its headers have come, and nothing reads its
  // body.
}

void PreflightLoader::OnSSLCertificateError(net::URLRequest* request,
                                            int net_error,
                                            const net::SSLInfo& ssl_info,
                                            bool fatal) {
  request->CancelWithError(net_error);
}

void PreflightLoader::Complete(int net_error) {
  if (callback_) {
    std::move(callback_).Run(net_error);
  }
}

}  // namespace mitto
