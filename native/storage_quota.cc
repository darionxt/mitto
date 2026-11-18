#include "mitto/storage_quota.h"

#include <algorithm>
#include <utility>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/rand_util.h"
#include "base/system/sys_info.h"
#include "base/task/task_runner.h"

namespace mitto {
namespace {

constexpr int64_t kGiB = 1024 * 1024 * 1024;

// kPoolSizeRatio, and kDefaultPerStorageKeyRatio.
constexpr double kPoolSizeRatio = 0.8;
constexpr double kPerStorageKeyRatio = 0.75;

// Incognito's pool, a random share of the memory, and each key's third.
constexpr double kIncognitoRatioLowerBound = 0.15;
constexpr double kIncognitoRatioUpperBound = 0.2;

// QuotaManagerImpl::kTypicalBucketUsage: a storage key may have as many
// buckets as its quota holds of these, and at least one.
constexpr int64_t kTypicalBucketUsage = 20 * 1024 * 1024;

// kMinutesAfterStartupToBeginEviction and kEvictionInterval.
constexpr base::TimeDelta kFirstEviction = base::Minutes(5);
constexpr base::TimeDelta kEvictionInterval = base::Minutes(30);

// CalculateReportedQuota(), less the usage: 10 GiB on a disk of 10 GiB or more
// and in memory, and otherwise the disk rounded up to a GiB.
int64_t ReportedQuotaExtra(std::optional<int64_t> total_disk) {
  if (!total_disk || *total_disk >= 10 * kGiB) {
    return 10 * kGiB;
  }
  return (*total_disk / kGiB + (*total_disk % kGiB != 0)) * kGiB;
}

bool HasExpired(const BucketInfo& bucket) {
  return !bucket.expiration.is_null() && bucket.expiration <= base::Time::Now();
}

}  // namespace

struct StorageQuota::KeyState {
  KeyState() = default;
  KeyState(KeyState&&) = default;
  KeyState& operator=(KeyState&&) = default;
  ~KeyState() = default;

  Usage usage;
  // How many clients have not said their usage yet; 0 once all have.
  size_t pending_clients = 0;
  bool asked = false;
  // Changes that came while the clients were asked, which their answers may
  // or may not include: the answers win, as QuotaManager's usage tracker
  // starts from what its clients report.
  std::vector<base::OnceClosure> waiting;
};

StorageQuota::BucketInitParams::BucketInitParams() = default;
StorageQuota::BucketInitParams::BucketInitParams(const BucketInitParams&) =
    default;
StorageQuota::BucketInitParams& StorageQuota::BucketInitParams::operator=(
    const BucketInitParams&) = default;
StorageQuota::BucketInitParams::~BucketInitParams() = default;

StorageQuota::StorageQuota(
    std::optional<base::FilePath> directory,
    uint64_t physical_memory,
    scoped_refptr<base::SequencedTaskRunner> file_runner,
    scoped_refptr<base::SequencedTaskRunner> database_runner)
    : directory_(std::move(directory)),
      file_runner_(std::move(file_runner)),
      database_runner_(database_runner),
      buckets_(directory_, std::move(database_runner)) {
  if (!directory_) {
    // CalculateIncognitoDynamicSettings().
    const double ratio = kIncognitoRatioLowerBound +
                         base::RandDouble() * (kIncognitoRatioUpperBound -
                                               kIncognitoRatioLowerBound);
    const int64_t pool =
        static_cast<int64_t>(static_cast<double>(physical_memory) * ratio);
    per_key_quota_ = pool / 3;
    reported_quota_extra_ = ReportedQuotaExtra(std::nullopt);
  }
}

StorageQuota::~StorageQuota() = default;

void StorageQuota::Open(base::OnceCallback<void(bool)> opened) {
  buckets_.Open(base::BindOnce(
      [](base::WeakPtr<StorageQuota> self,
         base::OnceCallback<void(bool)> opened, bool result) {
        if (!self) {
          return;
        }
        if (result) {
          self->first_eviction_.Start(
              FROM_HERE, kFirstEviction,
              base::BindOnce(
                  [](StorageQuota* quota) {
                    quota->EvictExpiredBuckets();
                    quota->eviction_.Start(
                        FROM_HERE, kEvictionInterval,
                        base::BindRepeating(&StorageQuota::EvictExpiredBuckets,
                                            base::Unretained(quota)));
                  },
                  base::Unretained(self.get())));
        }
        std::move(opened).Run(result);
      },
      weak_factory_.GetWeakPtr(), std::move(opened)));
}

void StorageQuota::RegisterClient(Client& client) {
  clients_.push_back(&client);
}

void StorageQuota::WhenQuotaKnown(base::OnceClosure done) {
  if (per_key_quota_) {
    std::move(done).Run();
    return;
  }
  waiting_for_quota_.push_back(std::move(done));
  if (asked_disk_) {
    return;
  }
  asked_disk_ = true;
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&base::SysInfo::AmountOfTotalDiskSpace, *directory_),
      base::BindOnce(&StorageQuota::DidGetTotalDiskSpace,
                     weak_factory_.GetWeakPtr()));
}

void StorageQuota::WhenKnown(std::vector<std::string> bucket_keys,
                             base::OnceClosure done) {
  WhenQuotaKnown(base::BindOnce(
      [](base::WeakPtr<StorageQuota> self, std::vector<std::string> bucket_keys,
         base::OnceClosure done) {
        if (!self) {
          return;
        }
        const base::RepeatingClosure barrier =
            base::BarrierClosure(bucket_keys.size(), std::move(done));
        for (const std::string& bucket_key : bucket_keys) {
          self->WhenBucketKnown(bucket_key, barrier);
        }
      },
      weak_factory_.GetWeakPtr(), std::move(bucket_keys), std::move(done)));
}

void StorageQuota::WhenBucketKnown(const std::string& bucket_key,
                                   base::OnceClosure done) {
  KeyState& key = usage_[bucket_key];
  if (key.asked && key.pending_clients == 0) {
    std::move(done).Run();
    return;
  }
  key.waiting.push_back(std::move(done));
  if (key.asked) {
    return;
  }
  key.asked = true;
  key.pending_clients = clients_.size();
  if (clients_.empty()) {
    std::vector<base::OnceClosure> waiting = std::move(key.waiting);
    for (base::OnceClosure& closure : waiting) {
      std::move(closure).Run();
    }
    return;
  }
  for (Client* client : clients_) {
    client->GetUsage(
        bucket_key,
        base::BindOnce(&StorageQuota::DidGetClientUsage,
                       weak_factory_.GetWeakPtr(), bucket_key, client->type()));
  }
}

void StorageQuota::DidGetTotalDiskSpace(std::optional<int64_t> total) {
  // CalculateNominalDynamicSettings(). A disk whose size cannot be read gets
  // no quota, so every write fails, as QuotaManager's error does.
  const int64_t total_disk = total.value_or(0);
  const int64_t pool =
      static_cast<int64_t>(static_cast<double>(total_disk) * kPoolSizeRatio);
  per_key_quota_ =
      static_cast<int64_t>(static_cast<double>(pool) * kPerStorageKeyRatio);
  reported_quota_extra_ = ReportedQuotaExtra(total);
  std::vector<base::OnceClosure> waiting = std::move(waiting_for_quota_);
  for (base::OnceClosure& closure : waiting) {
    std::move(closure).Run();
  }
}

void StorageQuota::DidGetClientUsage(const std::string& bucket_key,
                                     ClientType type,
                                     int64_t usage) {
  // A bucket that went while its clients were asked has no state any more.
  const auto found = usage_.find(bucket_key);
  if (found == usage_.end()) {
    return;
  }
  KeyState& key = found->second;
  switch (type) {
    case ClientType::kIndexedDatabase:
      key.usage.indexed_db = std::max<int64_t>(usage, 0);
      break;
    case ClientType::kServiceWorkerCache:
      key.usage.caches = std::max<int64_t>(usage, 0);
      break;
  }
  if (key.pending_clients == 0 || --key.pending_clients != 0) {
    return;
  }
  std::vector<base::OnceClosure> waiting = std::move(key.waiting);
  for (base::OnceClosure& closure : waiting) {
    std::move(closure).Run();
  }
}

std::vector<std::string> StorageQuota::BucketKeysOf(
    const std::string& storage_key) const {
  std::vector<std::string> bucket_keys = {storage_key};
  for (const BucketInfo* bucket : buckets_.BucketsOf(storage_key)) {
    if (!bucket->is_default()) {
      bucket_keys.push_back(BucketKey(*bucket));
    }
  }
  return bucket_keys;
}

StorageQuota::Usage StorageQuota::UsageOf(
    const std::vector<std::string>& bucket_keys) const {
  Usage total;
  for (const std::string& bucket_key : bucket_keys) {
    if (const auto found = usage_.find(bucket_key); found != usage_.end()) {
      total.indexed_db += found->second.usage.indexed_db;
      total.caches += found->second.usage.caches;
    }
  }
  return total;
}

void StorageQuota::GetUsageAndQuota(
    const std::string& storage_key,
    base::OnceCallback<void(UsageAndQuota)> done) {
  // GetStorageKeyUsageWithBreakdown(): what every bucket of the key holds.
  WhenKnown(
      BucketKeysOf(storage_key),
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, std::string storage_key,
             base::OnceCallback<void(UsageAndQuota)> done) {
            if (!self) {
              return;
            }
            const Usage usage = self->UsageOf(self->BucketKeysOf(storage_key));
            UsageAndQuota result;
            result.indexed_db = usage.indexed_db;
            result.caches = usage.caches;
            result.usage = usage.indexed_db + usage.caches;
            result.quota = self->per_key_quota_.value_or(0);
            result.reported_quota = result.usage + self->reported_quota_extra_;
            std::move(done).Run(result);
          },
          weak_factory_.GetWeakPtr(), storage_key, std::move(done)));
}

void StorageQuota::GetSpaceRemaining(
    const std::string& bucket_key,
    base::OnceCallback<void(std::optional<int64_t>)> done) {
  if (!IsBucketKeyLive(bucket_key)) {
    std::move(done).Run(std::nullopt);
    return;
  }
  const std::string storage_key(ParseBucketKey(bucket_key).first);
  WhenKnown(
      BucketKeysOf(storage_key),
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, std::string bucket_key,
             std::string storage_key,
             base::OnceCallback<void(std::optional<int64_t>)> done) {
            if (!self) {
              return;
            }
            if (!self->IsBucketKeyLive(bucket_key)) {
              std::move(done).Run(std::nullopt);
              return;
            }
            // GetBucketSpaceRemaining(): the least of what the storage key's
            // quota leaves and, for a bucket of a name, what its own quota,
            // at most the key's, leaves.
            const int64_t per_key = self->per_key_quota_.value_or(0);
            const Usage key_usage =
                self->UsageOf(self->BucketKeysOf(storage_key));
            int64_t remaining =
                per_key - key_usage.indexed_db - key_usage.caches;
            if (const std::optional<int64_t> id =
                    ParseBucketKey(bucket_key).second) {
              const BucketInfo& bucket = *self->buckets_.FindById(*id);
              const int64_t bucket_quota =
                  bucket.quota > 0 ? std::min(per_key, bucket.quota) : per_key;
              const Usage usage = self->UsageOf({bucket_key});
              remaining = std::min(
                  remaining, bucket_quota - usage.indexed_db - usage.caches);
            }
            std::move(done).Run(remaining);
          },
          weak_factory_.GetWeakPtr(), bucket_key, storage_key,
          std::move(done)));
}

void StorageQuota::NotifyModified(const std::string& bucket_key,
                                  ClientType type,
                                  int64_t delta) {
  const auto key = usage_.find(bucket_key);
  // A bucket whose usage nobody has asked for yet gets it from its clients
  // when someone does.
  if (key == usage_.end() || !key->second.asked ||
      key->second.pending_clients != 0) {
    return;
  }
  int64_t& usage = type == ClientType::kIndexedDatabase
                       ? key->second.usage.indexed_db
                       : key->second.usage.caches;
  usage = std::max<int64_t>(usage + delta, 0);
}

void StorageQuota::UpdateOrCreateBucket(
    BucketInitParams params,
    base::OnceCallback<void(base::expected<BucketInfo, BucketError>)> done) {
  // An expiration that has passed fails before anything else, even for a
  // bucket that is there.
  if (!params.expiration.is_null() && params.expiration <= base::Time::Now()) {
    std::move(done).Run(base::unexpected(BucketError::kInvalidExpiration));
    return;
  }
  // DidGetQuotaSettingsForBucketCreation(): the quota sets how many buckets a
  // storage key may have.
  WhenQuotaKnown(base::BindOnce(&StorageQuota::CreateBucket,
                                weak_factory_.GetWeakPtr(), std::move(params),
                                std::move(done)));
}

void StorageQuota::CreateBucket(
    BucketInitParams params,
    base::OnceCallback<void(base::expected<BucketInfo, BucketError>)> done) {
  if (const BucketInfo* existing =
          buckets_.Find(params.storage_key, params.name)) {
    // DidGetBucketCheckExpiration(): one that has expired goes with its data,
    // and a new one takes its name.
    if (HasExpired(*existing)) {
      const BucketInfo expired = *existing;
      DeleteBucketData(
          expired, base::BindOnce(&StorageQuota::UpdateOrCreateBucket,
                                  weak_factory_.GetWeakPtr(), std::move(params),
                                  std::move(done)));
      return;
    }
    // QuotaDatabase::UpdateOrCreateBucket(): a quota or a durability never
    // changes, and persistence only with a permission that a Page never has.
    if (!params.expiration.is_null() &&
        params.expiration != existing->expiration) {
      buckets_.SetExpiration(existing->id, params.expiration);
    }
    std::move(done).Run(*buckets_.FindById(existing->id));
    return;
  }
  // The default bucket counts among them, once there is one.
  const size_t max_buckets = static_cast<size_t>(
      std::max<int64_t>(1, *per_key_quota_ / kTypicalBucketUsage));
  if (buckets_.BucketsOf(params.storage_key).size() >= max_buckets) {
    std::move(done).Run(base::unexpected(BucketError::kQuotaExceeded));
    return;
  }
  BucketInfo bucket;
  bucket.storage_key = std::move(params.storage_key);
  bucket.name = std::move(params.name);
  bucket.expiration = params.expiration;
  bucket.quota = params.quota;
  bucket.durability = params.durability.value_or(BucketDurability::kRelaxed);
  std::move(done).Run(buckets_.Add(std::move(bucket)));
}

void StorageQuota::EnsureDefaultBucket(const std::string& storage_key) {
  if (buckets_.Find(storage_key, kDefaultBucketName)) {
    return;
  }
  BucketInfo bucket;
  bucket.storage_key = storage_key;
  bucket.name = kDefaultBucketName;
  buckets_.Add(std::move(bucket));
}

std::optional<BucketInfo> StorageQuota::GetBucketById(int64_t id) const {
  const BucketInfo* const bucket = buckets_.FindById(id);
  if (!bucket) {
    return std::nullopt;
  }
  return *bucket;
}

void StorageQuota::GetBucketNames(
    const std::string& storage_key,
    base::OnceCallback<void(std::vector<std::string>)> done) {
  DeleteExpiredBuckets(
      storage_key,
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, std::string storage_key,
             base::OnceCallback<void(std::vector<std::string>)> done) {
            if (!self) {
              return;
            }
            std::vector<std::string> names;
            for (const BucketInfo* bucket :
                 self->buckets_.BucketsOf(storage_key)) {
              if (!bucket->is_default()) {
                names.push_back(bucket->name);
              }
            }
            std::move(done).Run(std::move(names));
          },
          weak_factory_.GetWeakPtr(), storage_key, std::move(done)));
}

void StorageQuota::DeleteBucket(const std::string& storage_key,
                                const std::string& name,
                                base::OnceClosure done) {
  // FindAndDeleteBucketData() looks the bucket up on the database's sequence
  // first, so that what a Page sent just before, such as the commit of a
  // transaction of the bucket, comes first.
  database_runner_->PostTaskAndReply(
      FROM_HERE, base::DoNothing(),
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, std::string storage_key,
             std::string name, base::OnceClosure done) {
            if (!self) {
              return;
            }
            // One that is not there has been deleted, as far as the page
            // goes.
            const BucketInfo* const bucket =
                self->buckets_.Find(storage_key, name);
            if (!bucket) {
              std::move(done).Run();
              return;
            }
            self->DeleteBucketData(BucketInfo(*bucket), std::move(done));
          },
          weak_factory_.GetWeakPtr(), storage_key, name, std::move(done)));
}

std::optional<BucketInfo> StorageQuota::UpdateBucketExpiration(
    int64_t id,
    base::Time expiration) {
  // Any time, even one that has passed: the bucket then waits to be deleted.
  if (!buckets_.FindById(id)) {
    return std::nullopt;
  }
  buckets_.SetExpiration(id, expiration);
  return *buckets_.FindById(id);
}

void StorageQuota::GetBucketUsageAndReportedQuota(
    int64_t id,
    base::OnceCallback<void(std::optional<UsageAndQuota>)> done) {
  const BucketInfo* const bucket = buckets_.FindById(id);
  if (!bucket) {
    std::move(done).Run(std::nullopt);
    return;
  }
  WhenKnown(
      {BucketKey(*bucket)},
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, int64_t id,
             base::OnceCallback<void(std::optional<UsageAndQuota>)> done) {
            if (!self) {
              return;
            }
            const BucketInfo* const bucket = self->buckets_.FindById(id);
            if (!bucket) {
              std::move(done).Run(std::nullopt);
              return;
            }
            const Usage usage = self->UsageOf({BucketKey(*bucket)});
            UsageAndQuota result;
            result.indexed_db = usage.indexed_db;
            result.caches = usage.caches;
            result.usage = usage.indexed_db + usage.caches;
            result.quota = self->per_key_quota_.value_or(0);
            // A bucket's own quota, whatever its usage and its key's quota.
            result.reported_quota =
                bucket->quota > 0 ? bucket->quota
                                  : result.usage + self->reported_quota_extra_;
            std::move(done).Run(result);
          },
          weak_factory_.GetWeakPtr(), id, std::move(done)));
}

bool StorageQuota::IsBucketKeyLive(const std::string& bucket_key) const {
  const auto [storage_key, id] = ParseBucketKey(bucket_key);
  if (!id) {
    return true;
  }
  const BucketInfo* const bucket = buckets_.FindById(*id);
  return bucket && bucket->storage_key == storage_key;
}

void StorageQuota::DeleteBucketData(const BucketInfo& bucket,
                                    base::OnceClosure done) {
  // The bucket's name is free at once, so that a page that opens it again
  // gets a new bucket, while the clients let go of the old one's data.
  const std::string bucket_key = BucketKey(bucket);
  buckets_.Remove(bucket.id);
  const base::RepeatingClosure barrier = base::BarrierClosure(
      clients_.size(),
      base::BindOnce(
          [](base::WeakPtr<StorageQuota> self, std::string bucket_key,
             base::OnceClosure done) {
            if (self) {
              // What waited for the bucket's usage finds it gone.
              if (auto state = self->usage_.extract(bucket_key)) {
                for (base::OnceClosure& closure : state.mapped().waiting) {
                  std::move(closure).Run();
                }
              }
            }
            std::move(done).Run();
          },
          weak_factory_.GetWeakPtr(), bucket_key, std::move(done)));
  for (Client* client : clients_) {
    client->DeleteBucketData(bucket_key, barrier);
  }
}

void StorageQuota::DeleteExpiredBuckets(
    const std::optional<std::string>& storage_key,
    base::OnceClosure done) {
  std::vector<BucketInfo> expired;
  auto consider = [&](const BucketInfo& bucket) {
    if (!bucket.is_default() && HasExpired(bucket)) {
      expired.push_back(bucket);
    }
  };
  if (storage_key) {
    for (const BucketInfo* bucket : buckets_.BucketsOf(*storage_key)) {
      consider(*bucket);
    }
  } else {
    buckets_.ForEach(consider);
  }
  const base::RepeatingClosure barrier =
      base::BarrierClosure(expired.size(), std::move(done));
  for (const BucketInfo& bucket : expired) {
    DeleteBucketData(bucket, barrier);
  }
}

void StorageQuota::EvictExpiredBuckets() {
  DeleteExpiredBuckets(std::nullopt, base::DoNothing());
}

}  // namespace mitto
