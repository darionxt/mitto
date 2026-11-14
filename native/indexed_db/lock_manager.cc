#include "mitto/indexed_db/lock_manager.h"

#include <algorithm>
#include <memory>

#include "base/barrier_closure.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace mitto {

IndexedDbLock::IndexedDbLock(base::WeakPtr<IndexedDbLockManager> manager,
                             IndexedDbLockId id)
    : manager_(std::move(manager)), id_(std::move(id)) {}

IndexedDbLock::IndexedDbLock(IndexedDbLock&& other)
    : manager_(std::move(other.manager_)), id_(std::move(other.id_)) {
  other.manager_.reset();
}

IndexedDbLock& IndexedDbLock::operator=(IndexedDbLock&& other) {
  if (this != &other) {
    if (manager_) {
      manager_->LockReleased(id_);
    }
    manager_ = std::move(other.manager_);
    id_ = std::move(other.id_);
    other.manager_.reset();
  }
  return *this;
}

IndexedDbLock::~IndexedDbLock() {
  if (manager_) {
    manager_->LockReleased(id_);
  }
}

IndexedDbLockHolder::IndexedDbLockHolder() = default;
IndexedDbLockHolder::~IndexedDbLockHolder() = default;

IndexedDbLockManager::Lock::Lock() = default;
IndexedDbLockManager::Lock::Lock(Lock&&) = default;
IndexedDbLockManager::Lock& IndexedDbLockManager::Lock::operator=(Lock&&) =
    default;
IndexedDbLockManager::Lock::~Lock() = default;

IndexedDbLockManager::IndexedDbLockManager() = default;
IndexedDbLockManager::~IndexedDbLockManager() = default;

void IndexedDbLockManager::AcquireLocks(std::vector<LockRequest> requests,
                                        IndexedDbLockHolder& holder,
                                        base::OnceClosure acquired) {
  // A flat_set's order: by ID, then type, each ID once.
  std::sort(requests.begin(), requests.end(),
            [](const LockRequest& left, const LockRequest& right) {
              return left.id < right.id;
            });
  requests.erase(
      std::unique(requests.begin(), requests.end(),
                  [](const LockRequest& left, const LockRequest& right) {
                    return left.id == right.id;
                  }),
      requests.end());
  auto synchronous = std::make_shared<bool>(true);
  const base::RepeatingClosure barrier = base::BarrierClosure(
      requests.size(),
      base::BindOnce(
          [](std::shared_ptr<bool> synchronous,
             base::WeakPtr<IndexedDbLockHolder> holder,
             base::OnceClosure acquired) {
            if (!holder) {
              return;
            }
            if (*synchronous) {
              std::move(acquired).Run();
            } else {
              base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                  FROM_HERE, std::move(acquired));
            }
          },
          synchronous, holder.weak_factory_.GetWeakPtr(), std::move(acquired)));
  for (LockRequest& request : requests) {
    AcquireLock(std::move(request), holder, barrier);
  }
  *synchronous = false;
}

std::vector<IndexedDbLockId> IndexedDbLockManager::GetUnacquirableLocks(
    const std::vector<LockRequest>& requests) const {
  std::vector<IndexedDbLockId> ids;
  for (const LockRequest& request : requests) {
    const auto found = locks_.find(request.id);
    if (found != locks_.end() && !found->second.CanBeAcquired(request.type)) {
      ids.push_back(request.id);
    }
  }
  return ids;
}

bool IndexedDbLockManager::IsBlockingAnyRequest(
    const std::vector<IndexedDbLockId>& held) const {
  for (const IndexedDbLockId& id : held) {
    const auto found = locks_.find(id);
    if (found == locks_.end()) {
      continue;
    }
    for (const Waiting& waiting : found->second.queue) {
      if (waiting.holder) {
        return true;
      }
    }
  }
  return false;
}

void IndexedDbLockManager::AcquireLock(LockRequest request,
                                       IndexedDbLockHolder& holder,
                                       base::OnceClosure acquired) {
  Lock& lock = locks_[request.id];
  if (lock.CanBeAcquired(request.type)) {
    ++lock.acquired_count;
    lock.mode = request.type;
    holder.locks.emplace_back(weak_factory_.GetWeakPtr(), request.id);
    std::move(acquired).Run();
    return;
  }
  lock.queue.push_back(
      {request.type, holder.weak_factory_.GetWeakPtr(), std::move(acquired)});
}

void IndexedDbLockManager::LockReleased(const IndexedDbLockId& id) {
  const auto found = locks_.find(id);
  CHECK(found != locks_.end());
  Lock& lock = found->second;
  CHECK_GT(lock.acquired_count, 0);
  --lock.acquired_count;
  if (lock.acquired_count != 0) {
    return;
  }
  while (!lock.queue.empty() &&
         (lock.acquired_count == 0 ||
          lock.queue.front().type == LockType::kShared)) {
    Waiting waiting = std::move(lock.queue.front());
    lock.queue.pop_front();
    if (!waiting.holder) {
      continue;
    }
    ++lock.acquired_count;
    lock.mode = waiting.type;
    waiting.holder->locks.emplace_back(weak_factory_.GetWeakPtr(), id);
    std::move(waiting.acquired).Run();
    if (waiting.type == LockType::kExclusive) {
      return;
    }
  }
  if (lock.acquired_count == 0 && lock.queue.empty()) {
    locks_.erase(found);
  }
}

}  // namespace mitto
