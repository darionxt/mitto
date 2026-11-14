#include "mitto/storage/storage_context.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"

namespace mitto {

namespace {

// localStorage's, as Chrome's createOptions() has them on desktop.
StorageAreaImpl::Options LocalStorageOptions() {
  StorageAreaImpl::Options options;
  options.max_size = kPerStorageAreaQuota + kPerStorageAreaOverQuotaAllowance;
  // A moment after a change, for others to come in the same batch, and at most
  // an area's quota and 60 commits in an hour, so as not to wear the disk.
  options.default_commit_delay = base::Seconds(5);
  options.max_bytes_per_hour = kPerStorageAreaQuota;
  options.max_commits_per_hour = 60;
  return options;
}

}  // namespace

// An area of sessionStorage, which keeps no usage and is never written.
class SessionStorageNamespace::Holder final : public StorageAreaImpl::Delegate {
 public:
  explicit Holder(const StorageKey& storage_key)
      // The same quota as localStorage's, and nothing to write.
      : area_(nullptr, storage_key, this, LocalStorageOptions()) {
    area_.InitializeAsEmpty();
  }

  StorageAreaImpl& area() { return area_; }

  // StorageAreaImpl::Delegate:
  void OnNoBindings() override {}
  void AddUsageToCommit(LocalStorageDatabase::MapUpdate& update) override {}
  void DidCommit(bool succeeded) override {}

 private:
  StorageAreaImpl area_;
};

SessionStorageNamespace::SessionStorageNamespace() = default;
SessionStorageNamespace::~SessionStorageNamespace() = default;

StorageAreaImpl& SessionStorageNamespace::Area(const StorageKey& storage_key) {
  std::unique_ptr<Holder>& holder = areas_[storage_key];
  if (!holder) {
    holder = std::make_unique<Holder>(storage_key);
  }
  return holder->area();
}

// An area of localStorage, with the usage that Chrome writes beside it.
class StorageContext::LocalAreaHolder final : public StorageAreaImpl::Delegate {
 public:
  LocalAreaHolder(StorageContext& context, const StorageKey& storage_key)
      : context_(context),
        storage_key_(storage_key),
        area_(&context.local_storage_database_,
              storage_key,
              this,
              LocalStorageOptions()) {}

  ~LocalAreaHolder() override {
    // When the area was last opened, unless a commit wrote it already or the
    // area is empty.
    if (has_written_access_metadata_ || area_.empty()) {
      return;
    }
    context_->local_storage_database_
        .AsyncCall(base::IgnoreResult(&LocalStorageDatabase::PutLastAccessed))
        .WithArgs(std::vector<StorageKey>{storage_key_}, base::Time::Now());
  }

  StorageAreaImpl& area() { return area_; }

  // Whether nothing holds the area any more: no Page, no load and no commit.
  bool IsUnused() const {
    return !area_.has_bindings() && !area_.has_pending_load_tasks() &&
           !area_.HasPendingCommit() && !area_.HasCommitsInFlight();
  }

  // StorageAreaImpl::Delegate:
  void OnNoBindings() override {
    area_.ScheduleImmediateCommit();
    context_->PurgeIfUnused(storage_key_);
  }

  void AddUsageToCommit(LocalStorageDatabase::MapUpdate& update) override {
    if (area_.empty()) {
      update.delete_usage = true;
      return;
    }
    const base::Time now = base::Time::Now();
    update.last_modified = now;
    update.total_size = area_.storage_used();
    if (!has_written_access_metadata_) {
      update.last_accessed = now;
      has_written_access_metadata_ = true;
    }
  }

  void DidCommit(bool succeeded) override {
    context_->PurgeIfUnused(storage_key_);
  }

 private:
  const raw_ref<StorageContext> context_;
  const StorageKey storage_key_;
  bool has_written_access_metadata_ = false;
  StorageAreaImpl area_;
};

StorageContext::StorageContext(std::optional<base::FilePath> directory,
                               DirectoryLock lock)
    : directory_(std::move(directory)),
      lock_(std::move(lock)),
      // Its writes have to finish even when the process ends, as Chrome's
      // storage service's do.
      blocking_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      local_storage_database_(blocking_task_runner_) {}

StorageContext::~StorageContext() {
  // Each area writes what it has left, and then the database closes, after
  // those writes, on its own sequence.
  for (auto& [storage_key, holder] : local_areas_) {
    holder->area().ScheduleImmediateCommit();
  }
  local_areas_.clear();
  local_storage_database_.Reset();
  blocking_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](DirectoryLock lock, base::OnceClosure closed) {
                       // The directory is free before anyone hears of it.
                       {
                         DirectoryLock released = std::move(lock);
                       }
                       if (closed) {
                         std::move(closed).Run();
                       }
                     },
                     std::move(lock_), std::move(closed_)));
}

void StorageContext::Open(base::OnceCallback<void(bool)> opened) {
  base::FilePath path;
  if (directory_) {
    path = directory_->Append(kLocalStorageFileName);
  }
  local_storage_database_.AsyncCall(&LocalStorageDatabase::Open)
      .WithArgs(std::move(path))
      .Then(std::move(opened));
}

StorageAreaImpl& StorageContext::LocalStorageArea(
    const StorageKey& storage_key) {
  std::unique_ptr<LocalAreaHolder>& holder = local_areas_[storage_key];
  if (!holder) {
    holder = std::make_unique<LocalAreaHolder>(*this, storage_key);
  }
  return holder->area();
}

void StorageContext::PurgeIfUnused(const StorageKey& storage_key) {
  const auto found = local_areas_.find(storage_key);
  if (found == local_areas_.end() || !found->second->IsUnused()) {
    return;
  }
  // The holder may be what called this, so it goes from a task of its own.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<StorageContext> context, StorageKey storage_key) {
            if (!context) {
              return;
            }
            const auto found = context->local_areas_.find(storage_key);
            if (found != context->local_areas_.end() &&
                found->second->IsUnused()) {
              context->local_areas_.erase(found);
            }
          },
          weak_ptr_factory_.GetWeakPtr(), storage_key));
}

}  // namespace mitto
