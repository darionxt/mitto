// A Session's Cache API: the caches of each storage key, which every Page of
// the Session shares, as Chrome's CacheStorageManager keeps a profile's. In a
// directory, each storage key's caches go into CacheStorage/ there, in a
// directory named after a hash of the key; without one, they are kept in
// memory until the Session goes.
//
// Upstream: content/browser/cache_storage/cache_storage_manager.cc

#ifndef MITTO_CACHE_STORAGE_CACHE_STORAGE_CONTEXT_H_
#define MITTO_CACHE_STORAGE_CACHE_STORAGE_CONTEXT_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "mitto/storage_quota.h"

namespace mitto {

class CacheStorage;

class CacheStorageContext final : public StorageQuota::Client {
 public:
  // `directory` is the Session's storage directory, or nullopt to keep the
  // caches in memory.
  CacheStorageContext(std::optional<base::FilePath> directory,
                      StorageQuota& quota);
  CacheStorageContext(const CacheStorageContext&) = delete;
  CacheStorageContext& operator=(const CacheStorageContext&) = delete;
  ~CacheStorageContext() override;

  // The caches of `storage_key`, as StorageKey::Serialize() writes it.
  CacheStorage& ForKey(const std::string& storage_key);

  // Closes every cache, and calls `done` once each has written all it holds.
  // Nothing may use the caches after.
  void Close(base::OnceClosure done);

  // StorageQuota::Client:
  StorageQuota::ClientType type() const override;
  void GetUsage(const std::string& storage_key,
                base::OnceCallback<void(int64_t)> done) override;

 private:
  class KeyQuota;
  struct Key;

  const std::optional<base::FilePath> directory_;
  const raw_ref<StorageQuota> quota_;
  const scoped_refptr<base::SequencedTaskRunner> file_runner_;
  std::map<std::string, std::unique_ptr<Key>> keys_;
  base::WeakPtrFactory<CacheStorageContext> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_CACHE_STORAGE_CONTEXT_H_
