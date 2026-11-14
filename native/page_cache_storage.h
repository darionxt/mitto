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
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/cache_storage/cache_storage_cache.h"
#include "mitto/cache_storage_messages.h"

namespace mitto {

class CacheStorageContext;
class StorageQuota;

class PageCacheStorage {
 public:
  using Send = base::RepeatingCallback<void(std::string frame)>;
  using Refuse = base::RepeatingCallback<void(std::string why)>;
  // The storage key of the Page's document, or nullopt if its origin may not
  // reach the Cache API.
  using StorageKey = base::RepeatingCallback<std::optional<std::string>()>;

  PageCacheStorage(CacheStorageContext& context,
                   StorageQuota& quota,
                   Send send,
                   Refuse refuse,
                   StorageKey storage_key);
  PageCacheStorage(const PageCacheStorage&) = delete;
  PageCacheStorage& operator=(const PageCacheStorage&) = delete;
  ~PageCacheStorage();

  void OnCall(PageCacheStorageCall call);

 private:
  class PutBody;
  struct StoredBody;

  // Checks that the question's number goes up, and that the document may
  // reach the Cache API, and returns its storage key.
  std::optional<std::string> Admit(uint64_t question);
  CacheStorageCache* FindCache(uint64_t cache);

  void OnOpen(PageCacheStorageCall call, const std::string& storage_key);
  void OnMatch(PageCacheStorageCall call, const std::string& storage_key);
  void OnCacheBatch(PageCacheStorageCall call, CacheStorageCache& cache);
  void OnBodyPart(PageCacheStorageCall call);
  void OnReadBody(uint64_t body);
  void OnBodyCredit(uint64_t body, uint32_t credit);

  // Answers a question with the responses that matched, each body that has
  // bytes kept for the Page to read.
  void AnswerResponses(uint64_t question,
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
  const StorageKey storage_key_;
  uint64_t last_question_ = 0;
  // The caches the Page has open, by the number the library gave each.
  std::map<uint64_t, CacheStorageCacheHandle> caches_;
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
