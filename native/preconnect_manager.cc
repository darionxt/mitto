#include "mitto/preconnect_manager.h"

#include <optional>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/http_user_agent_settings.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/privacy_mode.h"
#include "net/base/request_priority.h"
#include "net/dns/host_resolver.h"  // nogncheck
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_request_info.h"
#include "net/http/http_stream_factory.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/transport_security_state.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/proxy_resolution/proxy_resolution_request.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_network_hints_preconnect",
        "A connection that a link of rel preconnect of a Page's document "
        "hints that the document will want.");

}  // namespace

// PreresolveJob, with what it waits for.
class PreconnectManager::Job {
 public:
  Job(GURL url,
      int num_sockets,
      bool allow_credentials,
      net::NetworkAnonymizationKey key)
      : url_(std::move(url)),
        num_sockets_(num_sockets),
        allow_credentials_(allow_credentials),
        key_(std::move(key)) {}
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  ~Job() = default;

  const GURL& url() const { return url_; }
  int num_sockets() const { return num_sockets_; }
  bool allow_credentials() const { return allow_credentials_; }
  const net::NetworkAnonymizationKey& key() const { return key_; }

  net::ProxyInfo& proxy_info() { return proxy_info_; }
  std::unique_ptr<net::ProxyResolutionRequest>& proxy_request() {
    return proxy_request_;
  }
  std::unique_ptr<net::HostResolver::ResolveHostRequest>& resolve_request() {
    return resolve_request_;
  }

 private:
  const GURL url_;
  const int num_sockets_;
  const bool allow_credentials_;
  const net::NetworkAnonymizationKey key_;
  net::ProxyInfo proxy_info_;
  std::unique_ptr<net::ProxyResolutionRequest> proxy_request_;
  std::unique_ptr<net::HostResolver::ResolveHostRequest> resolve_request_;
};

PreconnectManager::PreconnectManager(net::URLRequestContext& context)
    : context_(context) {}

PreconnectManager::~PreconnectManager() = default;

void PreconnectManager::StartPreresolveHost(
    const GURL& url,
    const net::NetworkAnonymizationKey& key) {
  // kAllowCredentialsOnPreconnectByDefault, which no connection uses.
  Queue(url, /*num_sockets=*/0, /*allow_credentials=*/true, key);
}

void PreconnectManager::StartPreconnectUrl(
    const GURL& url,
    bool allow_credentials,
    const net::NetworkAnonymizationKey& key) {
  Queue(url, /*num_sockets=*/1, allow_credentials, key);
}

void PreconnectManager::Queue(const GURL& url,
                              int num_sockets,
                              bool allow_credentials,
                              const net::NetworkAnonymizationKey& key) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  auto job = std::make_unique<Job>(url.DeprecatedGetOriginAsURL(), num_sockets,
                                   allow_credentials, key);
  // In front of the queue, as the latest hint is the most urgent.
  queued_jobs_.push_front(job.get());
  jobs_.insert(std::move(job));
  TryToLaunchJobs();
}

void PreconnectManager::TryToLaunchJobs() {
  while (!queued_jobs_.empty() &&
         inflight_preresolves_count_ < kMaxInflightPreresolves) {
    Job* const job = queued_jobs_.front();
    queued_jobs_.pop_front();
    ++inflight_preresolves_count_;
    // LookupProxyForUrl(), so that no name is resolved where a fixed proxy
    // serves the origin, as ProxyLookupRequest asks with no method.
    const int result = context_->proxy_resolution_service()->ResolveProxy(
        job->url(), std::string(), job->key(),
        net::handles::kInvalidNetworkHandle, &job->proxy_info(),
        base::BindOnce(
            [](base::WeakPtr<PreconnectManager> manager, Job* job, int result) {
              if (manager) {
                manager->OnProxyLookupFinished(
                    job, result == net::OK && !job->proxy_info().is_direct());
              }
            },
            weak_factory_.GetWeakPtr(), job),
        &job->proxy_request(), net::NetLogWithSource(), net::DEFAULT_PRIORITY);
    if (result != net::ERR_IO_PENDING) {
      // The answer reaches the browser from the network service later, as
      // it does for a lookup that waits.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&PreconnectManager::OnProxyLookupFinished,
                         weak_factory_.GetWeakPtr(), job,
                         result == net::OK && !job->proxy_info().is_direct()));
    }
  }
}

void PreconnectManager::OnProxyLookupFinished(Job* job, bool has_proxy) {
  job->proxy_request().reset();
  if (has_proxy) {
    FinishJob(job, /*success=*/true);
    return;
  }
  // ResolveHostClientImpl: a speculative resolution at the lowest priority,
  // of the scheme too, as an HTTPS record may upgrade the scheme.
  net::HostResolver::ResolveHostParameters parameters;
  parameters.initial_priority = net::IDLE;
  parameters.is_speculative = true;
  job->resolve_request() = context_->host_resolver()->CreateRequest(
      url::SchemeHostPort(job->url()), job->key(),
      net::handles::kInvalidNetworkHandle, net::NetLogWithSource(), parameters);
  const int result = job->resolve_request()->Start(base::BindOnce(
      [](base::WeakPtr<PreconnectManager> manager, Job* job, int result) {
        if (manager) {
          manager->OnPreresolveFinished(job, result == net::OK);
        }
      },
      weak_factory_.GetWeakPtr(), job));
  if (result != net::ERR_IO_PENDING) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PreconnectManager::OnPreresolveFinished,
                       weak_factory_.GetWeakPtr(), job, result == net::OK));
  }
}

void PreconnectManager::OnPreresolveFinished(Job* job, bool success) {
  job->resolve_request().reset();
  FinishJob(job, success);
}

void PreconnectManager::FinishJob(Job* job, bool success) {
  if (success && job->num_sockets() > 0) {
    PreconnectUrl(job->url(), job->num_sockets(), job->allow_credentials(),
                  job->key());
  }
  const auto found = jobs_.find(job);
  CHECK(found != jobs_.end());
  jobs_.erase(found);
  --inflight_preresolves_count_;
  TryToLaunchJobs();
}

void PreconnectManager::PreconnectUrl(const GURL& original_url,
                                      int num_sockets,
                                      bool allow_credentials,
                                      const net::NetworkAnonymizationKey& key) {
  // GetHSTSRedirectForPreconnect(): an HTTP origin that HSTS upgrades for a
  // subresource connects over HTTPS.
  GURL url = original_url;
  net::TransportSecurityState* const transport_security_state =
      context_->transport_security_state();
  if (transport_security_state && url.SchemeIs(url::kHttpScheme) &&
      transport_security_state->ShouldUpgradeToSSL(
          url.host(),
          /*is_top_level_nav=*/false)) {
    GURL::Replacements replacements;
    replacements.SetSchemeStr(url::kHttpsScheme);
    url = url.ReplaceComponents(replacements);
  }
  std::string user_agent;
  if (const net::HttpUserAgentSettings* const settings =
          context_->http_user_agent_settings()) {
    user_agent = settings->GetUserAgent();
  }
  net::HttpRequestInfo request_info;
  request_info.url = url;
  request_info.method = net::HttpRequestHeaders::kGetMethod;
  request_info.extra_headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                                       user_agent);
  request_info.traffic_annotation =
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation);
  if (allow_credentials) {
    request_info.load_flags = net::LOAD_NORMAL;
    request_info.privacy_mode = net::PRIVACY_MODE_DISABLED;
  } else {
    request_info.load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
    request_info.privacy_mode = net::PRIVACY_MODE_ENABLED;
  }
  request_info.network_anonymization_key = key;
  context_->http_transaction_factory()
      ->GetSession()
      ->http_stream_factory()
      ->PreconnectStreams(num_sockets, request_info, base::OnceClosure());
}

}  // namespace mitto
