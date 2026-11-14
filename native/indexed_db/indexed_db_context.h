// A Session's IndexedDB: the databases of each storage key, which every Page
// of the Session shares, as Chrome's IndexedDBContextImpl keeps a profile's,
// one bucket per storage key. Without a directory, each storage key's
// databases are kept in memory until the Session goes, as in Incognito.
//
// Upstream: content/browser/indexed_db/indexed_db_context_impl.cc

#ifndef MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_
#define MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "mitto/storage_quota.h"

namespace mitto {

class IndexedDbBucket;

class IndexedDbContext final : public StorageQuota::Client {
 public:
  explicit IndexedDbContext(StorageQuota& quota);
  IndexedDbContext(const IndexedDbContext&) = delete;
  IndexedDbContext& operator=(const IndexedDbContext&) = delete;
  ~IndexedDbContext() override;

  // The databases of `storage_key`, as StorageKey::Serialize() writes it.
  IndexedDbBucket& ForKey(const std::string& storage_key);

  // Closes every connection, as the end of the Session does. Nothing may use
  // the databases after.
  void Close();

  // StorageQuota::Client:
  StorageQuota::ClientType type() const override;
  void GetUsage(const std::string& storage_key,
                base::OnceCallback<void(int64_t)> done) override;

 private:
  class KeyQuota;

  const raw_ref<StorageQuota> quota_;
  std::map<std::string, std::unique_ptr<IndexedDbBucket>> buckets_;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_
