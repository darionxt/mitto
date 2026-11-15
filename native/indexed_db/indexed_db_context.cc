#include "mitto/indexed_db/indexed_db_context.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "mitto/indexed_db/backing_store.h"
#include "mitto/indexed_db/engine.h"
#include "mitto/indexed_db/kv_store.h"

namespace mitto {

// What a storage key's databases ask of the Session's quota.
class IndexedDbContext::KeyQuota final : public IndexedDbQuota {
 public:
  KeyQuota(StorageQuota& quota, std::string storage_key)
      : quota_(quota), storage_key_(std::move(storage_key)) {}

  // BucketContext::CheckCanUseDiskSpace().
  void CheckCanUseDiskSpace(int64_t bytes,
                            base::OnceCallback<void(bool)> done) override {
    if (bytes <= 0) {
      std::move(done).Run(true);
      return;
    }
    quota_->GetSpaceRemaining(
        storage_key_,
        base::BindOnce(
            [](int64_t bytes, base::OnceCallback<void(bool)> done,
               std::optional<int64_t> space_remaining) {
              std::move(done).Run(space_remaining && *space_remaining >= bytes);
            },
            bytes, std::move(done)));
  }

  void NotifyModified(int64_t delta) override {
    quota_->NotifyModified(storage_key_,
                           StorageQuota::ClientType::kIndexedDatabase, delta);
  }

 private:
  const raw_ref<StorageQuota> quota_;
  const std::string storage_key_;
};

IndexedDbContext::IndexedDbContext(StorageQuota& quota) : quota_(quota) {
  quota_->RegisterClient(*this);
}

IndexedDbContext::~IndexedDbContext() = default;

IndexedDbBucket& IndexedDbContext::ForKey(const std::string& storage_key) {
  std::unique_ptr<IndexedDbBucket>& bucket = buckets_[storage_key];
  if (!bucket) {
    bucket = std::make_unique<IndexedDbBucket>(
        std::make_unique<IndexedDbBackingStore>(
            std::make_unique<MemoryKvStore>()),
        std::make_unique<KeyQuota>(*quota_, storage_key));
  }
  return *bucket;
}

void IndexedDbContext::Close() {
  for (auto& [storage_key, bucket] : buckets_) {
    bucket->ForceClose("The Session is closing.");
  }
}

StorageQuota::ClientType IndexedDbContext::type() const {
  return StorageQuota::ClientType::kIndexedDatabase;
}

void IndexedDbContext::GetUsage(const std::string& storage_key,
                                base::OnceCallback<void(int64_t)> done) {
  const auto found = buckets_.find(storage_key);
  std::move(done).Run(
      found == buckets_.end() ? 0 : found->second->backing_store().Size());
}

}  // namespace mitto
