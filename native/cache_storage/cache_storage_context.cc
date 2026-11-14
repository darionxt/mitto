#include "mitto/cache_storage/cache_storage_context.h"

#include <utility>

#include "base/barrier_closure.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "crypto/hash.h"
#include "mitto/cache_storage/cache_storage.h"
#include "mitto/cache_storage/cache_storage_cache.h"

namespace mitto {
namespace {

constexpr char kCacheStorageDirectoryName[] = "CacheStorage";

// The directory of a storage key's caches: a hash of the key, as Chrome names
// it after a hash of the origin.
std::string KeyDirectoryName(const std::string& storage_key) {
  return base::HexEncodeLower(crypto::hash::Sha256(storage_key));
}

}  // namespace

// What a storage key's caches ask of the Session's quota.
class CacheStorageContext::KeyQuota final : public CacheStorageQuota {
 public:
  KeyQuota(StorageQuota& quota, std::string storage_key)
      : quota_(quota), storage_key_(std::move(storage_key)) {}

  void GetSpaceRemaining(
      base::OnceCallback<void(std::optional<int64_t>)> done) override {
    quota_->GetSpaceRemaining(storage_key_, std::move(done));
  }

  void NotifyModified(int64_t delta) override {
    quota_->NotifyModified(storage_key_,
                           StorageQuota::ClientType::kServiceWorkerCache,
                           delta);
  }

 private:
  const raw_ref<StorageQuota> quota_;
  const std::string storage_key_;
};

struct CacheStorageContext::Key {
  Key(StorageQuota& session_quota,
      const std::string& storage_key,
      base::FilePath directory,
      scoped_refptr<base::SequencedTaskRunner> file_runner)
      : quota(session_quota, storage_key),
        caches(std::move(directory), quota, std::move(file_runner)) {}

  KeyQuota quota;
  CacheStorage caches;
};

CacheStorageContext::CacheStorageContext(
    std::optional<base::FilePath> directory,
    StorageQuota& quota)
    : directory_(std::move(directory)),
      quota_(quota),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  quota_->RegisterClient(*this);
}

CacheStorageContext::~CacheStorageContext() = default;

CacheStorage& CacheStorageContext::ForKey(const std::string& storage_key) {
  std::unique_ptr<Key>& key = keys_[storage_key];
  if (!key) {
    base::FilePath directory;
    if (directory_) {
      directory = directory_->AppendASCII(kCacheStorageDirectoryName)
                      .AppendASCII(KeyDirectoryName(storage_key));
    }
    key = std::make_unique<Key>(*quota_, storage_key, std::move(directory),
                                file_runner_);
  }
  return key->caches;
}

void CacheStorageContext::Close(base::OnceClosure done) {
  if (keys_.empty()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             std::move(done));
    return;
  }
  const base::RepeatingClosure barrier =
      base::BarrierClosure(keys_.size(), std::move(done));
  for (auto& [storage_key, key] : keys_) {
    key->caches.Close(barrier);
  }
}

StorageQuota::ClientType CacheStorageContext::type() const {
  return StorageQuota::ClientType::kServiceWorkerCache;
}

void CacheStorageContext::GetUsage(const std::string& storage_key,
                                   base::OnceCallback<void(int64_t)> done) {
  ForKey(storage_key).Size(std::move(done));
}

}  // namespace mitto
