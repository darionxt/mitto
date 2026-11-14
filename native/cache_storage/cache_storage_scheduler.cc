#include "mitto/cache_storage/cache_storage_scheduler.h"

#include "base/check.h"
#include "base/check_op.h"

namespace mitto {
namespace {

// kCacheStorageParallelOps, which is on, and its max_shared_ops.
constexpr int kMaxSharedOperations = 16;

}  // namespace

CacheStorageScheduler::CacheStorageScheduler() = default;
CacheStorageScheduler::~CacheStorageScheduler() = default;

void CacheStorageScheduler::ScheduleOperation(int64_t id,
                                              Mode mode,
                                              base::OnceClosure operation) {
  // Every operation has the normal priority, so they run in the order of
  // their IDs, which is the order they came in.
  pending_.push_back({id, mode, std::move(operation)});
  MaybeRunOperation();
}

void CacheStorageScheduler::CompleteOperationAndRunNext(int64_t id) {
  const auto running = running_.find(id);
  CHECK(running != running_.end());
  if (running->second == Mode::kShared) {
    CHECK_GT(running_shared_, 0);
    --running_shared_;
  } else {
    CHECK_EQ(running_exclusive_, 1);
    --running_exclusive_;
  }
  running_.erase(running);
  MaybeRunOperation();
}

void CacheStorageScheduler::MaybeRunOperation() {
  // Running an operation can complete it and come back here: the loop below
  // runs the next one then.
  if (in_maybe_run_) {
    return;
  }
  in_maybe_run_ = true;
  const base::WeakPtr<CacheStorageScheduler> self = weak_factory_.GetWeakPtr();
  while (self && !pending_.empty()) {
    if (running_exclusive_ > 0) {
      break;
    }
    const bool shared = pending_.front().mode == Mode::kShared;
    if (running_shared_ >= (shared ? kMaxSharedOperations : 1)) {
      break;
    }
    Operation next = std::move(pending_.front());
    pending_.pop_front();
    running_.emplace(next.id, next.mode);
    if (shared) {
      ++running_shared_;
    } else {
      ++running_exclusive_;
    }
    // May destroy this.
    std::move(next.run).Run();
  }
  if (self) {
    in_maybe_run_ = false;
  }
}

}  // namespace mitto
