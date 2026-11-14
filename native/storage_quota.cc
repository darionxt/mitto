#include "mitto/storage_quota.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
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

// CalculateReportedQuota(), less the usage: 10 GiB on a disk of 10 GiB or more
// and in memory, and otherwise the disk rounded up to a GiB.
int64_t ReportedQuotaExtra(std::optional<int64_t> total_disk) {
  if (!total_disk || *total_disk >= 10 * kGiB) {
    return 10 * kGiB;
  }
  return (*total_disk / kGiB + (*total_disk % kGiB != 0)) * kGiB;
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

StorageQuota::StorageQuota(std::optional<base::FilePath> directory,
                           uint64_t physical_memory,
                           scoped_refptr<base::SequencedTaskRunner> file_runner)
    : directory_(std::move(directory)), file_runner_(std::move(file_runner)) {
  if (!directory_) {
    // CalculateIncognitoDynamicSettings().
    const double ratio =
        kIncognitoRatioLowerBound +
        base::RandDouble() *
            (kIncognitoRatioUpperBound - kIncognitoRatioLowerBound);
    const int64_t pool =
        static_cast<int64_t>(static_cast<double>(physical_memory) * ratio);
    per_key_quota_ = pool / 3;
    reported_quota_extra_ = ReportedQuotaExtra(std::nullopt);
  }
}

StorageQuota::~StorageQuota() = default;

void StorageQuota::RegisterClient(Client& client) {
  clients_.push_back(&client);
}

void StorageQuota::WhenKnown(const std::string& storage_key,
                             base::OnceClosure done) {
  if (!per_key_quota_) {
    waiting_for_quota_.push_back(base::BindOnce(
        &StorageQuota::WhenKnown, weak_factory_.GetWeakPtr(), storage_key,
        std::move(done)));
    if (!asked_disk_) {
      asked_disk_ = true;
      file_runner_->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&base::SysInfo::AmountOfTotalDiskSpace, *directory_),
          base::BindOnce(&StorageQuota::DidGetTotalDiskSpace,
                         weak_factory_.GetWeakPtr()));
    }
    return;
  }
  KeyState& key = keys_[storage_key];
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
    client->GetUsage(storage_key,
                     base::BindOnce(&StorageQuota::DidGetClientUsage,
                                    weak_factory_.GetWeakPtr(), storage_key,
                                    client->type()));
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

void StorageQuota::DidGetClientUsage(const std::string& storage_key,
                                     ClientType type,
                                     int64_t usage) {
  KeyState& key = keys_[storage_key];
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

StorageQuota::UsageAndQuota StorageQuota::Compute(
    const std::string& storage_key) const {
  UsageAndQuota result;
  if (const auto key = keys_.find(storage_key); key != keys_.end()) {
    result.indexed_db = key->second.usage.indexed_db;
    result.caches = key->second.usage.caches;
  }
  result.usage = result.indexed_db + result.caches;
  result.quota = per_key_quota_.value_or(0);
  result.reported_quota = result.usage + reported_quota_extra_;
  return result;
}

void StorageQuota::GetUsageAndQuota(
    const std::string& storage_key,
    base::OnceCallback<void(UsageAndQuota)> done) {
  WhenKnown(storage_key,
            base::BindOnce(
                [](base::WeakPtr<StorageQuota> self, std::string storage_key,
                   base::OnceCallback<void(UsageAndQuota)> done) {
                  if (self) {
                    std::move(done).Run(self->Compute(storage_key));
                  }
                },
                weak_factory_.GetWeakPtr(), storage_key, std::move(done)));
}

void StorageQuota::GetSpaceRemaining(
    const std::string& storage_key,
    base::OnceCallback<void(std::optional<int64_t>)> done) {
  GetUsageAndQuota(storage_key,
                   base::BindOnce(
                       [](base::OnceCallback<void(std::optional<int64_t>)> done,
                          UsageAndQuota usage_and_quota) {
                         std::move(done).Run(usage_and_quota.quota -
                                             usage_and_quota.usage);
                       },
                       std::move(done)));
}

void StorageQuota::NotifyModified(const std::string& storage_key,
                                  ClientType type,
                                  int64_t delta) {
  const auto key = keys_.find(storage_key);
  // A key whose usage nobody has asked for yet gets it from its clients
  // when someone does.
  if (key == keys_.end() || !key->second.asked ||
      key->second.pending_clients != 0) {
    return;
  }
  int64_t& usage = type == ClientType::kIndexedDatabase
                       ? key->second.usage.indexed_db
                       : key->second.usage.caches;
  usage = std::max<int64_t>(usage + delta, 0);
}

}  // namespace mitto
