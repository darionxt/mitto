// Runs the operations of a cache, or of a storage key's list of caches, in
// the order they came: any number of shared ones at once, up to 16 as
// kCacheStorageParallelOps allows, and an exclusive one alone.
//
// Upstream: content/browser/cache_storage/cache_storage_scheduler.cc

#ifndef MITTO_CACHE_STORAGE_CACHE_STORAGE_SCHEDULER_H_
#define MITTO_CACHE_STORAGE_CACHE_STORAGE_SCHEDULER_H_

#include <stdint.h>

#include <map>
#include <utility>

#include "base/containers/circular_deque.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace mitto {

class CacheStorageScheduler {
 public:
  enum class Mode {
    kShared,
    kExclusive,
  };

  CacheStorageScheduler();
  CacheStorageScheduler(const CacheStorageScheduler&) = delete;
  CacheStorageScheduler& operator=(const CacheStorageScheduler&) = delete;
  ~CacheStorageScheduler();

  int64_t CreateId() { return next_id_++; }

  // Queues `operation`, and runs it at once if nothing stops it. It must end
  // with CompleteOperationAndRunNext(id).
  void ScheduleOperation(int64_t id, Mode mode, base::OnceClosure operation);
  void CompleteOperationAndRunNext(int64_t id);

  bool ScheduledOperations() const {
    return !running_.empty() || !pending_.empty();
  }
  bool IsRunningExclusiveOperation() const { return running_exclusive_ > 0; }

  // Wraps `callback` to also complete operation `id`.
  template <typename... Args>
  base::OnceCallback<void(Args...)> WrapCallbackToRunNext(
      int64_t id,
      base::OnceCallback<void(Args...)> callback) {
    return base::BindOnce(
        [](base::WeakPtr<CacheStorageScheduler> scheduler, int64_t id,
           base::OnceCallback<void(Args...)> callback, Args... args) {
          // The callback may destroy what owns the scheduler.
          std::move(callback).Run(std::forward<Args>(args)...);
          if (scheduler) {
            scheduler->CompleteOperationAndRunNext(id);
          }
        },
        weak_factory_.GetWeakPtr(), id, std::move(callback));
  }

 private:
  struct Operation {
    int64_t id = 0;
    Mode mode = Mode::kShared;
    base::OnceClosure run;
  };

  void MaybeRunOperation();

  int64_t next_id_ = 0;
  base::circular_deque<Operation> pending_;
  std::map<int64_t, Mode> running_;
  int running_shared_ = 0;
  int running_exclusive_ = 0;
  bool in_maybe_run_ = false;
  base::WeakPtrFactory<CacheStorageScheduler> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_CACHE_STORAGE_SCHEDULER_H_
