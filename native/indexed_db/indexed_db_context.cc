#include "mitto/indexed_db/indexed_db_context.h"

#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "mitto/indexed_db/backing_store.h"
#include "mitto/indexed_db/engine.h"
#include "mitto/indexed_db/kv_store.h"
#include "mitto/storage/storage_key.h"

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

IndexedDbContext::IndexedDbContext(
    StorageQuota& quota,
    std::optional<base::FilePath> directory,
    scoped_refptr<base::SequencedTaskRunner> file_runner)
    : quota_(quota),
      directory_(std::move(directory)),
      file_runner_(std::move(file_runner)) {
  quota_->RegisterClient(*this);
}

IndexedDbContext::~IndexedDbContext() = default;

void IndexedDbContext::Open(base::OnceCallback<void(bool)> opened) {
  if (!directory_) {
    std::move(opened).Run(true);
    return;
  }
  file_ = base::SequenceBound<IndexedDbFile>(file_runner_);
  file_.AsyncCall(&IndexedDbFile::Open)
      .WithArgs(directory_->Append(kIndexedDbFileName))
      .Then(std::move(opened));
}

void IndexedDbContext::WithBucket(
    const std::string& storage_key,
    base::OnceCallback<void(IndexedDbBucket&)> use) {
  const auto found = buckets_.find(storage_key);
  if (found != buckets_.end()) {
    std::move(use).Run(*found->second);
    return;
  }
  if (!KeptInFile(storage_key)) {
    std::move(use).Run(AddBucket(storage_key, KvEntries(),
                                 /*write_through=*/false));
    return;
  }
  auto [waiting, first] = loading_.try_emplace(storage_key);
  waiting->second.push_back(std::move(use));
  if (!first) {
    return;
  }
  file_.AsyncCall(&IndexedDbFile::Read)
      .WithArgs(storage_key)
      .Then(base::BindOnce(&IndexedDbContext::Loaded,
                           weak_factory_.GetWeakPtr(), storage_key));
}

void IndexedDbContext::Close(base::OnceClosure done) {
  for (auto& [storage_key, bucket] : buckets_) {
    bucket->ForceClose("The Session is closing.");
  }
  if (!file_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             std::move(done));
    return;
  }
  // The file closes on its sequence after the writes before it, and `done`
  // runs after that.
  file_.Reset();
  file_runner_->PostTaskAndReply(FROM_HERE, base::DoNothing(), std::move(done));
}

StorageQuota::ClientType IndexedDbContext::type() const {
  return StorageQuota::ClientType::kIndexedDatabase;
}

void IndexedDbContext::GetUsage(const std::string& storage_key,
                                base::OnceCallback<void(int64_t)> done) {
  const auto found = buckets_.find(storage_key);
  if (found != buckets_.end()) {
    std::move(done).Run(found->second->backing_store().Size());
    return;
  }
  if (!KeptInFile(storage_key)) {
    std::move(done).Run(0);
    return;
  }
  // What the file holds, which every commit before reaches first, without
  // loading the databases.
  file_.AsyncCall(&IndexedDbFile::Usage)
      .WithArgs(storage_key)
      .Then(base::BindOnce(
          [](base::OnceCallback<void(int64_t)> done,
             std::optional<int64_t> usage) {
            std::move(done).Run(usage.value_or(0));
          },
          std::move(done)));
}

bool IndexedDbContext::KeptInFile(const std::string& storage_key) const {
  return file_ && !IsSerializedUnderOpaqueTop(storage_key);
}

IndexedDbBucket& IndexedDbContext::AddBucket(const std::string& storage_key,
                                             KvEntries entries,
                                             bool write_through) {
  MemoryKvStore::Writer writer;
  if (write_through) {
    writer = base::BindRepeating(&IndexedDbContext::Write,
                                 base::Unretained(this), storage_key);
  }
  std::unique_ptr<IndexedDbBucket>& bucket = buckets_[storage_key];
  CHECK(!bucket);
  bucket = std::make_unique<IndexedDbBucket>(
      std::make_unique<IndexedDbBackingStore>(std::make_unique<MemoryKvStore>(
          std::move(entries), std::move(writer))),
      std::make_unique<KeyQuota>(*quota_, storage_key));
  return *bucket;
}

void IndexedDbContext::Loaded(const std::string& storage_key,
                              std::optional<KvEntries> entries) {
  // A file that cannot be read leaves the databases of the storage key in
  // memory, for this Session, rather than writing over what it holds.
  const bool read = entries.has_value();
  IndexedDbBucket& bucket =
      AddBucket(storage_key, read ? *std::move(entries) : KvEntries(), read);
  std::vector<base::OnceCallback<void(IndexedDbBucket&)>> waiting =
      std::move(loading_.extract(storage_key).mapped());
  for (base::OnceCallback<void(IndexedDbBucket&)>& use : waiting) {
    std::move(use).Run(bucket);
  }
}

void IndexedDbContext::Write(const std::string& storage_key,
                             const KvBatch& batch) {
  // Nothing commits once the Session closes, which closes the file.
  if (!file_) {
    return;
  }
  // Chrome's commit fails when LevelDB cannot write. Here the transaction has
  // already completed, so a write that fails is lost.
  file_.AsyncCall(base::IgnoreResult(&IndexedDbFile::Write))
      .WithArgs(storage_key, batch);
}

}  // namespace mitto
