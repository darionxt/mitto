#include "mitto/storage/storage_area_impl.h"

#include <algorithm>
#include <iterator>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"

namespace mitto {

StorageAreaImpl::RateLimiter::RateLimiter(size_t desired_rate,
                                          base::TimeDelta time_quantum)
    : rate_(desired_rate), time_quantum_(time_quantum) {
  DCHECK_GT(desired_rate, 0u);
}

base::TimeDelta StorageAreaImpl::RateLimiter::ComputeDelayNeeded(
    base::TimeDelta elapsed_time) const {
  const base::TimeDelta time_needed = time_quantum_ * (samples_ / rate_);
  if (time_needed > elapsed_time) {
    return time_needed - elapsed_time;
  }
  return base::TimeDelta();
}

StorageAreaImpl::CommitBatch::CommitBatch() = default;
StorageAreaImpl::CommitBatch::~CommitBatch() = default;

StorageAreaImpl::OnLoadCompleteTask::OnLoadCompleteTask(
    base::OnceClosure callback,
    AccessMode mode)
    : callback(std::move(callback)), mode(mode) {}
StorageAreaImpl::OnLoadCompleteTask::OnLoadCompleteTask(OnLoadCompleteTask&&) =
    default;
StorageAreaImpl::OnLoadCompleteTask&
StorageAreaImpl::OnLoadCompleteTask::operator=(OnLoadCompleteTask&&) = default;
StorageAreaImpl::OnLoadCompleteTask::~OnLoadCompleteTask() = default;

StorageAreaImpl::StorageAreaImpl(
    base::SequenceBound<LocalStorageDatabase>* database,
    StorageKey storage_key,
    Delegate* delegate,
    const Options& options)
    : database_(database),
      storage_key_(std::move(storage_key)),
      delegate_(delegate),
      max_size_(options.max_size),
      start_time_(base::TimeTicks::Now()),
      default_commit_delay_(options.default_commit_delay),
      data_rate_limiter_(options.max_bytes_per_hour, base::Hours(1)),
      commit_rate_limiter_(options.max_commits_per_hour, base::Hours(1)) {}

StorageAreaImpl::~StorageAreaImpl() {
  CommitChanges();
}

void StorageAreaImpl::InitializeAsEmpty() {
  DCHECK_EQ(map_state_, MapState::kUnloaded);
  map_state_ = MapState::kLoading;
  OnMapLoaded(StorageValueMap());
}

void StorageAreaImpl::Bind() {
  ++bindings_;
}

void StorageAreaImpl::Unbind() {
  DCHECK_GT(bindings_, 0);
  --bindings_;
  NotifyNoBindingsIfIdle();
}

void StorageAreaImpl::NotifyNoBindingsIfIdle() {
  // A Page that let the area go may have left tasks that wait for its map,
  // which still have to run.
  if (bindings_ == 0 && on_load_complete_tasks_.empty()) {
    delegate_->OnNoBindings();
  }
}

bool StorageAreaImpl::has_pending_load_read_write_tasks() const {
  return std::ranges::any_of(on_load_complete_tasks_, [](const auto& task) {
    return task.mode == AccessMode::kReadWrite;
  });
}

void StorageAreaImpl::ScheduleImmediateCommit() {
  if (has_pending_load_read_write_tasks()) {
    LoadMap(OnLoadCompleteTask(
        base::BindOnce(&StorageAreaImpl::ScheduleImmediateCommit,
                       weak_ptr_factory_.GetWeakPtr()),
        AccessMode::kReadWrite));
    return;
  }
  CommitChanges();
}

void StorageAreaImpl::AddObserver(StorageAreaObserver* observer) {
  observers_.insert(observer);
}

void StorageAreaImpl::RemoveObserver(StorageAreaObserver* observer) {
  observers_.erase(observer);
}

void StorageAreaImpl::Put(const std::string& key,
                          const std::string& value,
                          const PageStorageSource& source) {
  if (map_state_ != MapState::kLoaded) {
    LoadMap(OnLoadCompleteTask(
        base::BindOnce(&StorageAreaImpl::Put, weak_ptr_factory_.GetWeakPtr(),
                       key, value, source),
        AccessMode::kReadWrite));
    return;
  }

  size_t old_item_size = 0;
  std::optional<std::string> old_value;
  const auto found = keys_values_map_.find(key);
  if (found != keys_values_map_.end()) {
    if (found->second == value) {
      // The key keeps its value, but each observer still hears of it, since
      // the writer counts on it.
      for (StorageAreaObserver* observer : std::set(observers_)) {
        observer->KeyChanged(key, value, value, source);
      }
      return;
    }
    old_value = found->second;
    old_item_size = key.size() + old_value->size();
  }

  const size_t new_item_size = key.size() + value.size();
  const size_t new_storage_used = storage_used_ - old_item_size + new_item_size;
  // Only a change that grows the area is checked, so that one over the quota
  // can still shrink.
  if (new_item_size > old_item_size && new_storage_used > max_size_) {
    for (StorageAreaObserver* observer : std::set(observers_)) {
      observer->KeyChangeFailed(key, source);
    }
    return;
  }

  if (database_) {
    CreateCommitBatchIfNeeded();
    commit_batch_->changed_keys.insert(key);
  }
  keys_values_map_[key] = value;
  storage_used_ = new_storage_used;
  for (StorageAreaObserver* observer : std::set(observers_)) {
    observer->KeyChanged(key, value, old_value, source);
  }
}

void StorageAreaImpl::Delete(const std::string& key,
                             const PageStorageSource& source) {
  if (map_state_ != MapState::kLoaded) {
    LoadMap(OnLoadCompleteTask(
        base::BindOnce(&StorageAreaImpl::Delete, weak_ptr_factory_.GetWeakPtr(),
                       key, source),
        AccessMode::kReadWrite));
    return;
  }
  const auto found = keys_values_map_.find(key);
  if (found == keys_values_map_.end()) {
    // Nothing changes, but each observer still hears of it.
    for (StorageAreaObserver* observer : std::set(observers_)) {
      observer->KeyDeleted(key, std::nullopt, source);
    }
    return;
  }
  if (database_) {
    CreateCommitBatchIfNeeded();
    commit_batch_->changed_keys.insert(key);
  }
  std::string old_value = std::move(found->second);
  keys_values_map_.erase(found);
  storage_used_ -= key.size() + old_value.size();
  for (StorageAreaObserver* observer : std::set(observers_)) {
    observer->KeyDeleted(key, old_value, source);
  }
}

void StorageAreaImpl::DeleteAll(
    const PageStorageSource& source,
    base::WeakPtr<StorageAreaObserver> new_observer) {
  if (map_state_ != MapState::kLoaded) {
    LoadMap(OnLoadCompleteTask(base::BindOnce(&StorageAreaImpl::DeleteAll,
                                              weak_ptr_factory_.GetWeakPtr(),
                                              source, std::move(new_observer)),
                               AccessMode::kReadWrite));
    return;
  }
  const bool already_empty = keys_values_map_.empty();
  if (new_observer) {
    AddObserver(new_observer.get());
  }
  if (already_empty) {
    for (StorageAreaObserver* observer : std::set(observers_)) {
      observer->AllDeleted(/*was_nonempty=*/false, source);
    }
    return;
  }
  if (database_) {
    CreateCommitBatchIfNeeded();
    commit_batch_->clear_all_first = true;
    commit_batch_->changed_keys.clear();
  }
  keys_values_map_.clear();
  storage_used_ = 0;
  for (StorageAreaObserver* observer : std::set(observers_)) {
    observer->AllDeleted(/*was_nonempty=*/true, source);
  }
}

void StorageAreaImpl::GetAll(base::WeakPtr<StorageAreaObserver> new_observer,
                             GetAllCallback callback) {
  if (map_state_ != MapState::kLoaded) {
    LoadMap(OnLoadCompleteTask(
        base::BindOnce(&StorageAreaImpl::GetAll, weak_ptr_factory_.GetWeakPtr(),
                       std::move(new_observer), std::move(callback)),
        AccessMode::kReadOnly));
    return;
  }
  std::vector<std::pair<std::string, std::string>> all(keys_values_map_.begin(),
                                                       keys_values_map_.end());
  std::move(callback).Run(std::move(all));
  if (new_observer) {
    AddObserver(new_observer.get());
  }
}

void StorageAreaImpl::LoadMap(OnLoadCompleteTask task) {
  DCHECK_NE(map_state_, MapState::kLoaded);
  on_load_complete_tasks_.push_back(std::move(task));
  if (map_state_ == MapState::kLoading) {
    return;
  }
  map_state_ = MapState::kLoading;
  if (!database_) {
    OnMapLoaded(std::nullopt);
    return;
  }
  database_->AsyncCall(&LocalStorageDatabase::ReadMap)
      .WithArgs(storage_key_)
      .Then(base::BindOnce(&StorageAreaImpl::OnMapLoaded,
                           weak_ptr_factory_.GetWeakPtr()));
}

void StorageAreaImpl::OnMapLoaded(std::optional<StorageValueMap> map) {
  CHECK(keys_values_map_.empty());
  CHECK_EQ(map_state_, MapState::kLoading);
  map_state_ = MapState::kLoaded;
  if (map) {
    keys_values_map_ = *std::move(map);
  } else {
    // As Chrome does, the area goes on without its database: nothing is
    // written, but it works for as long as it lives.
    database_ = nullptr;
  }
  storage_used_ = 0;
  for (const auto& [key, value] : keys_values_map_) {
    storage_used_ += key.size() + value.size();
  }
  OnLoadComplete();
}

void StorageAreaImpl::OnLoadComplete() {
  DCHECK_EQ(map_state_, MapState::kLoaded);
  std::vector<OnLoadCompleteTask> tasks;
  on_load_complete_tasks_.swap(tasks);
  for (OnLoadCompleteTask& task : tasks) {
    std::move(task.callback).Run();
  }
  NotifyNoBindingsIfIdle();
}

void StorageAreaImpl::CreateCommitBatchIfNeeded() {
  if (commit_batch_) {
    return;
  }
  DCHECK(database_);
  commit_batch_ = std::make_unique<CommitBatch>();
  StartCommitTimer();
}

void StorageAreaImpl::StartCommitTimer() {
  // While commits are in flight, the timer starts once they have finished.
  if (!commit_batch_ || commit_batches_in_flight_) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&StorageAreaImpl::CommitChanges,
                     weak_ptr_factory_.GetWeakPtr()),
      ComputeCommitDelay());
}

base::TimeDelta StorageAreaImpl::ComputeCommitDelay() const {
  const base::TimeDelta elapsed_time = base::TimeTicks::Now() - start_time_;
  return std::max({default_commit_delay_,
                   commit_rate_limiter_.ComputeDelayNeeded(elapsed_time),
                   data_rate_limiter_.ComputeDelayNeeded(elapsed_time)});
}

void StorageAreaImpl::CommitChanges() {
  // A commit that was scheduled may find that an immediate one came first.
  if (!commit_batch_ || !database_) {
    return;
  }
  DCHECK_EQ(map_state_, MapState::kLoaded);
  commit_rate_limiter_.add_samples(1);

  LocalStorageDatabase::MapUpdate update(storage_key_);
  update.clear_all_first = commit_batch_->clear_all_first;
  delegate_->AddUsageToCommit(update);
  size_t data_size = 0;
  for (const std::string& key : commit_batch_->changed_keys) {
    data_size += key.size();
    const auto found = keys_values_map_.find(key);
    if (found != keys_values_map_.end()) {
      data_size += found->second.size();
      update.entries_to_add.emplace_back(key, found->second);
    } else {
      update.keys_to_delete.push_back(key);
    }
  }
  data_rate_limiter_.add_samples(data_size);
  ++commit_batches_in_flight_;
  commit_batch_.reset();

  std::vector<LocalStorageDatabase::MapUpdate> updates;
  updates.push_back(std::move(update));
  database_->AsyncCall(&LocalStorageDatabase::UpdateMaps)
      .WithArgs(std::move(updates))
      .Then(base::BindOnce(&StorageAreaImpl::OnCommitComplete,
                           weak_ptr_factory_.GetWeakPtr()));
}

void StorageAreaImpl::OnCommitComplete(bool succeeded) {
  --commit_batches_in_flight_;
  StartCommitTimer();
  delegate_->DidCommit(succeeded);
}

}  // namespace mitto
