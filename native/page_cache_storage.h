// The library's side of a Page's Cache API, on the network thread: it serves
// what the Page's CacheStorage and Caches ask, as Chrome's
// CacheStorageDispatcherHost serves a renderer, from the Session's caches of
// the storage key of the Page's document, which the library works out
// itself. A put's body comes from the Page as the cache writes it, a part at
// a time, and a stored body goes to the Page as it gives credit for it, so
// that a Page never makes the library hold more than a part of either.
//
// Upstream: content/browser/cache_storage/cache_storage_dispatcher_host.cc

#ifndef MITTO_PAGE_CACHE_STORAGE_H_
#define MITTO_PAGE_CACHE_STORAGE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/cache_storage/cache_storage_cache.h"
#include "mitto/cache_storage_messages.h"
#include "mitto/cross_origin_policies.h"
#include "url/origin.h"

namespace mitto {

class CacheStorageContext;
class StorageQuota;

class PageCacheStorage {
 public:
  using Send = base::RepeatingCallback<void(std::string frame)>;
  using Refuse = base::RepeatingCallback<void(std::string why)>;

  // The numbers of the Page's caches and stored bodies start at
  // `first_number`, past those of the hosts of its documents before.
  PageCacheStorage(CacheStorageContext& context,
                   StorageQuota& quota,
                   Send send,
                   Refuse refuse,
                   uint64_t first_number = 1);
  PageCacheStorage(const PageCacheStorage&) = delete;
  PageCacheStorage& operator=(const PageCacheStorage&) = delete;
  ~PageCacheStorage();

  // A call of the Page's context `context`, whose puts fail when it goes.
  // CacheStorage's calls go to the bucket that `storage_key` names, as
  // BucketKey() gives it, and navigator.storage.estimate() to the storage
  // key, or nullopt if the context's origin may not reach the Cache API.
  // `bucket_live` says whether the bucket is still there.
  void OnCall(PageCacheStorageCall call,
              uint64_t context,
              const std::optional<std::string>& storage_key,
              bool bucket_live = true);

  // The origin, embedder policy and document isolation policy of the Page's
  // context `context`, which the responses that its matches give are held to.
  void SetContextPolicy(uint64_t context,
                        url::Origin origin,
                        CrossOriginEmbedderPolicy embedder_policy,
                        DocumentIsolationPolicy document_isolation_policy);

  // The first number that no cache or stored body has had.
  uint64_t NextNumber() const;

  // The Page's contexts `contexts` have gone: their puts that wait for a body
  // fail, as they do when the host goes, so that none holds its cache, and
  // the caches they opened and the stored bodies they could still read are
  // let go, as their pipes go with them in Chrome. What they asked that
  // answers later holds nothing and goes nowhere.
  void ForgetContexts(const std::set<uint64_t>& contexts);

 private:
  class PutBody;
  struct StoredBody;

  // Checks that the question's number goes up, and that the document may
  // reach the Cache API.
  bool Admit(uint64_t question, const std::optional<std::string>& storage_key);
  CacheStorageCache* FindCache(uint64_t cache, uint64_t context);
  // Whether `context` opened `cache`, or no context has it open; refuses the
  // Page otherwise.
  bool OpenedBy(uint64_t cache, uint64_t context);
  // Whether a cache's call names a cache of a bucket that has been deleted.
  bool CacheBucketGone(const PageCacheStorageCall& call) const;
  // Answers a call of a bucket that has been deleted, or of one of its caches.
  void AnswerGoneBucket(const PageCacheStorageCall& call, uint64_t context);

  void OnOpen(PageCacheStorageCall call,
              uint64_t context,
              const std::string& storage_key);
  void OnMatch(PageCacheStorageCall call,
               uint64_t context,
               const std::string& storage_key);
  void OnCacheBatch(PageCacheStorageCall call,
                    uint64_t context,
                    CacheStorageCache& cache);
  void OnBodyPart(PageCacheStorageCall call);
  void OnReadBody(uint64_t body);
  void OnBodyCredit(uint64_t body, uint32_t credit);

  // Answers a question with the responses that matched, each body that has
  // bytes kept for the Page to read.
  void AnswerResponses(uint64_t question,
                       uint64_t context,
                       CacheStorageError error,
                       std::vector<CacheStorageCache::MatchResult> matches);
  void SendAnswer(const PageCacheStorageAnswer& answer);

  // A put asks the Page for the next part of body `body`.
  void WantBody(PutBody& put_body, uint32_t most);
  void ForgetPutBody(uint64_t body);
  // Reads the next part of a stored body, while the Page has credit for it.
  void ReadMore(uint64_t body);
  void DidRead(uint64_t body, int rv);

  const raw_ref<CacheStorageContext> context_;
  const raw_ref<StorageQuota> quota_;
  const Send send_;
  const Refuse refuse_;
  uint64_t last_question_ = 0;
  // The caches the Page has open, by the number the library gave each.
  std::map<uint64_t, CacheStorageCacheHandle> caches_;
  // The key of the bucket of each, as BucketKey() gives it.
  std::map<uint64_t, std::string> cache_buckets_;
  // The context that opened each, which lets it go when it goes.
  std::map<uint64_t, uint64_t> cache_contexts_;
  // The contexts that have gone.
  std::set<uint64_t> gone_contexts_;
  // The origin and embedder policy of each context that has called.
  struct ContextPolicy {
    url::Origin origin;
    CrossOriginEmbedderPolicy embedder_policy;
    DocumentIsolationPolicy document_isolation_policy;
  };
  std::map<uint64_t, ContextPolicy> context_policies_;
  uint64_t next_cache_ = 1;
  // The bodies of puts that wait for the Page, by the Page's numbers.
  std::map<uint64_t, PutBody*> put_bodies_;
  // The stored bodies the Page may read, by the library's numbers.
  std::map<uint64_t, std::unique_ptr<StoredBody>> stored_bodies_;
  uint64_t next_stored_body_ = 1;
  base::WeakPtrFactory<PageCacheStorage> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_CACHE_STORAGE_H_
