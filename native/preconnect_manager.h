// What Chrome's browser does with the network hints of a page's documents,
// which Blink sends for a link of rel dns-prefetch or preconnect: its
// PreconnectManager, as NetworkHintsHandlerImpl uses it, with the default
// path of Chrome 157, PreconnectManagerDirectFastPath off. Each hint is a job
// of the queue, the newest first, three at a time. A job looks up the proxy
// for its origin, resolves the origin's host if it goes direct, at the lowest
// priority, and, for a preconnect, then opens one connection to it, as
// NetworkContext::PreconnectSockets() does, with the credentials that the
// hint allows.
//
// Upstream: chrome/browser/predictors/network_hints_handler_impl.cc
// Upstream: content/browser/preloading/preconnect/preconnect_manager_impl.cc
// Upstream: content/browser/preloading/proxy_lookup_client_impl.cc
// Upstream: content/browser/preloading/resolve_host_client_impl.cc
// Upstream: services/network/network_context.cc

#ifndef MITTO_PRECONNECT_MANAGER_H_
#define MITTO_PRECONNECT_MANAGER_H_

#include <stddef.h>

#include <memory>
#include <set>

#include "base/containers/circular_deque.h"
#include "base/containers/unique_ptr_adapters.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "net/base/network_anonymization_key.h"
#include "url/gurl.h"

namespace net {
class URLRequestContext;
}  // namespace net

namespace mitto {

class PreconnectManager {
 public:
  // As many jobs as look up a proxy or resolve a host at once.
  static constexpr size_t kMaxInflightPreresolves = 3;

  // Uses `context`, which outlives it.
  explicit PreconnectManager(net::URLRequestContext& context);

  PreconnectManager(const PreconnectManager&) = delete;
  PreconnectManager& operator=(const PreconnectManager&) = delete;

  ~PreconnectManager();

  // StartPreresolveHosts() for one URL: resolves the host of `url`, an HTTP
  // or HTTPS URL, unless a proxy serves it.
  void StartPreresolveHost(const GURL& url,
                           const net::NetworkAnonymizationKey& key);
  // StartPreconnectUrl(): resolves the host of `url`, an HTTP or HTTPS URL,
  // as StartPreresolveHost() does, and then opens a connection to its origin,
  // which carries credentials if `allow_credentials`.
  void StartPreconnectUrl(const GURL& url,
                          bool allow_credentials,
                          const net::NetworkAnonymizationKey& key);

 private:
  class Job;

  // Queues a job for `url`'s origin in front of the others, and launches what
  // it can.
  void Queue(const GURL& url,
             int num_sockets,
             bool allow_credentials,
             const net::NetworkAnonymizationKey& key);
  // TryToLaunchPreresolveJobs().
  void TryToLaunchJobs();
  // OnProxyLookupFinished() and OnPreresolveFinished(): a proxy serves the
  // job's origin, or its host resolved, or neither.
  void OnProxyLookupFinished(Job* job, bool has_proxy);
  void OnPreresolveFinished(Job* job, bool success);
  // FinishPreresolveJob(): preconnects for a preconnect's job that went
  // well, and lets the next job go.
  void FinishJob(Job* job, bool success);
  // PreconnectUrl() and NetworkContext::PreconnectSockets().
  void PreconnectUrl(const GURL& url,
                     int num_sockets,
                     bool allow_credentials,
                     const net::NetworkAnonymizationKey& key);

  const raw_ref<net::URLRequestContext> context_;
  std::set<std::unique_ptr<Job>, base::UniquePtrComparator> jobs_;
  base::circular_deque<Job*> queued_jobs_;
  size_t inflight_preresolves_count_ = 0;
  base::WeakPtrFactory<PreconnectManager> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PRECONNECT_MANAGER_H_
