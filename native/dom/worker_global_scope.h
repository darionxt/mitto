// The HTML Standard's WorkerGlobalScope, the global object of a worker's
// context, with `self`, `origin`, `isSecureContext` and what every
// ExecutionContext has, the timers, queueMicrotask(), atob(), btoa(),
// reportError() and structuredClone(); its other members are Known Gaps.
//
// As a window, a worker's context has its global object and its global proxy
// both wrap it, so that a call whose receiver is either finds it.
//
// Upstream: third_party/blink/renderer/core/workers/worker_global_scope.h
// Upstream: third_party/blink/renderer/core/workers/worker_or_worklet_global_scope.h
// Upstream: third_party/blink/renderer/bindings/core/v8/worker_or_worklet_script_controller.cc

#ifndef MITTO_DOM_WORKER_GLOBAL_SCOPE_H_
#define MITTO_DOM_WORKER_GLOBAL_SCOPE_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/fetcher.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CacheStorage;
class CacheStorageHost;
class IDBFactory;
class Modulator;
class IndexedDbHost;
class StorageBucketHost;
class Crypto;
class ImportedScriptLoader;
class Scheduler;
class WorkerLocation;
class WorkerNavigator;

class WorkerGlobalScope : public EventTarget, public ExecutionContext {
 public:
  ~WorkerGlobalScope() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  WorkerGlobalScope* Self() { return this; }
  // The worker's URL, made the first time.
  WorkerLocation* Location();
  // What the worker reads of the browser and the machine, made the first time.
  WorkerNavigator* Navigator();
  // Each made the first time.
  mitto::Crypto* Crypto();
  mitto::Scheduler* Scheduler();
  // GlobalCacheStorage::caches(): the worker's CacheStorage, made the first
  // time, or null after throwing a SecurityError for a worker whose origin
  // may not reach caches.
  mitto::CacheStorage* Caches(bindings::ExceptionState& exception_state);
  // GlobalIndexedDB::indexedDB(): the worker's IDBFactory, made the first
  // time, which a worker whose origin may not reach IndexedDB has too: its
  // calls throw.
  IDBFactory* IndexedDB();
  // The worker's origin, its creator's, serialized.
  std::u16string Origin() const override;
  bool IsSecureContextForBindings() const override;
  // WindowOrWorkerGlobalScope::crossOriginIsolated().
  bool CrossOriginIsolated() const { return CrossOriginIsolatedCapability(); }
  // WorkerPerformance, made the first time, which counts from when the worker
  // started.
  mitto::Performance* Performance() override;
  void ImportScripts(const std::vector<std::u16string>& urls,
                     bindings::ExceptionState& exception_state);
  v8::Local<v8::Value> StructuredClone(
      v8::Local<v8::Value> value,
      const bindings::StructuredSerializeOptions& options,
      bindings::ExceptionState& exception_state);

  // ExecutionContext:
  FetchReferrerPolicy GetReferrerPolicy() const override;
  std::u16string Url() const override;
  EventTarget* ErrorEventTarget() override;
  url::Origin GetSecurityOrigin() const override;
  url::Origin AgentClusterOrigin() const override;
  GURL CompleteUrl(std::u16string_view url) const override;
  GURL BaseUrl() const override;
  CacheStorageHost* GetCacheStorageHost() const override;
  IndexedDbHost* GetIndexedDbHost() const override;
  StorageBucketHost* GetStorageBucketHost() const override;
  Modulator* GetModulator() override;

  // What the Page uses.
  // Where the worker's Cache API reaches the Session: none for a worker of an
  // opaque origin, or once the worker has ended.
  void SetCacheStorageHost(CacheStorageHost* host) {
    cache_storage_host_ = host;
  }
  // Where the worker's IndexedDB reaches the Session, the same.
  void SetIndexedDbHost(IndexedDbHost* host) { indexed_db_host_ = host; }
  // Where the worker's Storage Buckets API reaches the Session, the same.
  void SetStorageBucketHost(StorageBucketHost* host) {
    storage_bucket_host_ = host;
  }
  // The worker's URL, its script's final one, as InitializeURL() takes it
  // once the script has come, and its origin, its creator's or an opaque one
  // for a data: URL, from when it starts.
  void SetUrl(GURL url) { url_ = std::move(url); }
  const GURL& GetUrl() const { return url_; }
  // The policy of the response of the worker's script, from when it has come.
  void SetReferrerPolicy(FetchReferrerPolicy policy) {
    referrer_policy_ = policy;
  }
  void SetOrigin(url::Origin origin) { origin_ = std::move(origin); }
  // Whether the worker runs a module script, whose importScripts() throws.
  void SetModule(bool module) { module_ = module; }
  // Its creator's referrer policy, that of the outside settings that its
  // top-level module graph is fetched with.
  void SetOutsideReferrerPolicy(FetchReferrerPolicy policy) {
    outside_referrer_policy_ = policy;
  }
  FetchReferrerPolicy OutsideReferrerPolicy() const {
    return outside_referrer_policy_;
  }
  // Its creator's origin, that of the same outside settings, which may read
  // its creator's blob: URLs where a worker of an opaque origin may not.
  void SetOutsideOrigin(url::Origin origin) {
    outside_origin_ = std::move(origin);
  }
  const url::Origin& OutsideOrigin() const { return outside_origin_; }
  // Its creator's AgentClusterOrigin().
  void SetAgentClusterOrigin(url::Origin origin) {
    agent_cluster_origin_ = std::move(origin);
  }
  // What importScripts() fetches through. Without one, as in the Page program
  // given a script, each script fails to load.
  void SetImportedScriptLoader(ImportedScriptLoader* loader) {
    imported_script_loader_ = loader;
  }
  // close(): the worker ends once the task that closed it ends.
  void Close() { closing_ = true; }
  bool IsClosing() const { return closing_; }
  // Makes the global proxy and the global object of `context`, which was made
  // from the template of this scope's interface, this scope's wrappers, as
  // Blink's WorkerOrWorkletScriptController::Initialize() does.
  void AssociateWithContext(v8::Isolate* isolate,
                            v8::Local<v8::Context> context);

 protected:
  WorkerGlobalScope();

 private:
  cppgc::Member<mitto::Performance> performance_;
  cppgc::Member<WorkerLocation> location_;
  cppgc::Member<WorkerNavigator> navigator_;
  cppgc::Member<mitto::Crypto> crypto_;
  cppgc::Member<mitto::Scheduler> scheduler_;
  cppgc::Member<mitto::CacheStorage> caches_;
  cppgc::Member<IDBFactory> indexed_db_;
  cppgc::Member<Modulator> modulator_;
  GURL url_;
  url::Origin origin_;
  raw_ptr<ImportedScriptLoader> imported_script_loader_ = nullptr;
  raw_ptr<CacheStorageHost> cache_storage_host_ = nullptr;
  raw_ptr<IndexedDbHost> indexed_db_host_ = nullptr;
  raw_ptr<StorageBucketHost> storage_bucket_host_ = nullptr;
  bool module_ = false;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
  FetchReferrerPolicy outside_referrer_policy_ = FetchReferrerPolicy::kDefault;
  url::Origin outside_origin_;
  url::Origin agent_cluster_origin_;
  bool closing_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_WORKER_GLOBAL_SCOPE_H_
