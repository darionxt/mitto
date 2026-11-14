// A CORS preflight, as Chrome's network service makes one for a request in
// cors mode that needs it: when it needs one, what it sends, what its response
// has to say, and the Session's cache of what earlier preflights allowed. The
// source export leaves services/network out, so this is ported with the
// upstream names kept.
//
// Upstream: services/network/cors/cors_url_loader.cc
// Upstream: services/network/cors/cors_util.cc
// Upstream: services/network/cors/preflight_cache.cc
// Upstream: services/network/cors/preflight_controller.cc
// Upstream: services/network/cors/preflight_result.cc
// Upstream: services/network/public/cpp/cors/cors.cc

#ifndef MITTO_CORS_PREFLIGHT_H_
#define MITTO_CORS_PREFLIGHT_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/containers/lru_cache.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "mitto/request_options.h"
#include "net/base/isolation_info.h"
#include "net/base/network_isolation_key.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/referrer_policy.h"
#include "net/url_request/url_request.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
class HttpResponseHeaders;
class URLRequestContext;
struct NetworkTrafficAnnotationTag;
}  // namespace net

namespace mitto {

// IsCorsSafelistedHeader() of the network service, which counts the client
// hints that Chrome adds as safelisted beside the Fetch Standard's names.
bool IsCorsSafelistedHeader(std::string_view name, std::string_view value);

// The names of `headers`, in lower case and in their order, that are neither
// forbidden nor CORS-safelisted. Past 1,024 bytes of safelisted values, the
// safelisted names count too.
std::vector<std::string> CorsUnsafeNotForbiddenRequestHeaderNames(
    const net::HttpRequestHeaders& headers);

// NeedsPreflight() for a request in cors mode: a method that is not
// safelisted, or a line that is neither forbidden nor safelisted.
bool NeedsPreflight(std::string_view method,
                    const net::HttpRequestHeaders& headers);

// CheckAccess(): whether a response's Access-Control-Allow-Origin and
// Access-Control-Allow-Credentials let a request of `origin` with these
// credentials read it.
bool CheckAccess(const GURL& response_url,
                 const net::HttpResponseHeaders& headers,
                 CredentialsMode credentials_mode,
                 const url::Origin& origin);

// What a preflight's response allows, until it expires.
class PreflightResult {
 public:
  // Returns nullptr if Access-Control-Allow-Methods or -Headers is not a list
  // of tokens.
  static std::unique_ptr<PreflightResult> Create(
      CredentialsMode credentials_mode,
      const net::HttpResponseHeaders& headers);

  PreflightResult(const PreflightResult&) = delete;
  PreflightResult& operator=(const PreflightResult&) = delete;

  ~PreflightResult();

  bool EnsureAllowedCrossOriginMethod(std::string_view method) const;
  bool EnsureAllowedCrossOriginHeaders(
      const net::HttpRequestHeaders& headers) const;
  bool EnsureAllowedRequest(CredentialsMode credentials_mode,
                            std::string_view method,
                            const net::HttpRequestHeaders& headers) const;
  bool IsExpired() const;

 private:
  explicit PreflightResult(CredentialsMode credentials_mode);

  base::flat_set<std::string> methods_;
  base::flat_set<std::string> headers_;
  const bool credentials_;
  base::TimeTicks absolute_expiry_time_;
};

// A Session's preflight results, by the request's origin, URL and network
// isolation key, partitioned by the top frame's site, as a network context
// keeps them.
class PreflightCache {
 public:
  PreflightCache();
  PreflightCache(const PreflightCache&) = delete;
  PreflightCache& operator=(const PreflightCache&) = delete;
  ~PreflightCache();

  void AppendEntry(const url::Origin& origin,
                   const GURL& url,
                   const net::NetworkIsolationKey& network_isolation_key,
                   std::unique_ptr<PreflightResult> result);

  // Whether a result that has not expired allows the request. One that has
  // expired or does not allow it goes.
  bool CheckIfRequestCanSkipPreflight(
      const url::Origin& origin,
      const GURL& url,
      const net::NetworkIsolationKey& network_isolation_key,
      CredentialsMode credentials_mode,
      std::string_view method,
      const net::HttpRequestHeaders& headers);

 private:
  using EntryKey =
      std::tuple<url::Origin, std::string, net::NetworkIsolationKey>;
  using EntryMap = std::map<EntryKey, std::unique_ptr<PreflightResult>>;

  base::LRUCache<std::optional<net::SchemefulSite>, EntryMap> cache_;
};

// What a preflight is made for: the request where it is about to go, at its
// start or at a redirect, as its rules have configured it.
struct PreflightRequest {
  PreflightRequest();
  PreflightRequest(const PreflightRequest&);
  PreflightRequest& operator=(const PreflightRequest&);
  ~PreflightRequest();

  GURL url;
  std::string method;
  net::HttpRequestHeaders headers;
  std::string referrer;
  net::ReferrerPolicy referrer_policy =
      net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN;
  url::Origin initiator;
  net::IsolationInfo isolation_info;
  net::RequestPriority priority = net::DEFAULT_PRIORITY;
  bool priority_incremental = net::kDefaultPriorityIncremental;
  int load_flags = 0;
  // The request's Sec-Fetch-Dest and credentials mode.
  std::string destination;
  CredentialsMode credentials_mode = CredentialsMode::kSameOrigin;
  // Whether a redirect has made the request's origin "null".
  bool tainted = false;
};

// Whether `cache` holds a result that lets `request` go without a preflight.
// A tainted request never reads it.
bool CanSkipPreflight(PreflightCache& cache, const PreflightRequest& request);

// The preflight of one request: CreatePreflightRequest() and
// PreflightController::PreflightLoader. It tells its callback net::OK if the
// request may go, and net::ERR_FAILED, or a net error of its own, if not.
class PreflightLoader final : public net::URLRequest::Delegate {
 public:
  using Callback = base::OnceCallback<void(int net_error)>;

  PreflightLoader(net::URLRequestContext& context,
                  const net::NetworkTrafficAnnotationTag& traffic_annotation,
                  const PreflightRequest& actual,
                  PreflightCache& cache,
                  Callback callback);

  PreflightLoader(const PreflightLoader&) = delete;
  PreflightLoader& operator=(const PreflightLoader&) = delete;

  ~PreflightLoader() override;

  void Start();

  // net::URLRequest::Delegate:
  void OnReceivedRedirect(net::URLRequest* request,
                          const net::RedirectInfo& redirect_info,
                          bool* defer_redirect) override;
  void OnResponseStarted(net::URLRequest* request, int net_error) override;
  void OnReadCompleted(net::URLRequest* request, int bytes_read) override;
  void OnSSLCertificateError(net::URLRequest* request,
                             int net_error,
                             const net::SSLInfo& ssl_info,
                             bool fatal) override;

 private:
  // Ends the preflight, and tells the callback once. This may delete the
  // loader, through its owner.
  void Complete(int net_error);

  const raw_ref<PreflightCache> cache_;
  const PreflightRequest actual_;
  std::unique_ptr<net::URLRequest> request_;
  Callback callback_;
};

}  // namespace mitto

#endif  // MITTO_CORS_PREFLIGHT_H_
