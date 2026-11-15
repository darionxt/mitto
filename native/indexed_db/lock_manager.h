// The locks of IndexedDB's transactions and connection requests, as Chrome's
// PartitionedLockManager grants them: each lock is shared or exclusive, a
// request waits behind those queued before it, and one that holds several
// gets them one at a time, in the order of their IDs, and hears once it holds
// all of them.
//
// Upstream: components/services/storage/indexed_db/locks/partitioned_lock_manager.cc

#ifndef MITTO_INDEXED_DB_LOCK_MANAGER_H_
#define MITTO_INDEXED_DB_LOCK_MANAGER_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace mitto {

class IndexedDbLockManager;

// The ID of a lock: a partition, and a key within it.
using IndexedDbLockId = std::pair<int, std::string>;

// One lock that a holder holds, which goes when this does.
class IndexedDbLock {
 public:
  IndexedDbLock(base::WeakPtr<IndexedDbLockManager> manager,
                IndexedDbLockId id);
  IndexedDbLock(IndexedDbLock&&);
  IndexedDbLock& operator=(IndexedDbLock&&);
  ~IndexedDbLock();

  const IndexedDbLockId& id() const { return id_; }

 private:
  base::WeakPtr<IndexedDbLockManager> manager_;
  IndexedDbLockId id_;
};

// What waits for, and then holds, the locks of one request.
class IndexedDbLockHolder {
 public:
  IndexedDbLockHolder();
  IndexedDbLockHolder(const IndexedDbLockHolder&) = delete;
  IndexedDbLockHolder& operator=(const IndexedDbLockHolder&) = delete;
  ~IndexedDbLockHolder();

  // Gives up the locks it waits for.
  void CancelLockRequest() { weak_factory_.InvalidateWeakPtrs(); }

  std::vector<IndexedDbLock> locks;

 private:
  friend class IndexedDbLockManager;
  base::WeakPtrFactory<IndexedDbLockHolder> weak_factory_{this};
};

class IndexedDbLockManager {
 public:
  enum class LockType {
    kShared,
    kExclusive,
  };
  struct LockRequest {
    IndexedDbLockId id;
    LockType type = LockType::kShared;
  };

  IndexedDbLockManager();
  IndexedDbLockManager(const IndexedDbLockManager&) = delete;
  IndexedDbLockManager& operator=(const IndexedDbLockManager&) = delete;
  ~IndexedDbLockManager();

  // Acquires `requests` into `holder`, and calls `acquired` once it holds
  // them all: at once if it got them at once, and otherwise from a task.
  void AcquireLocks(std::vector<LockRequest> requests,
                    IndexedDbLockHolder& holder,
                    base::OnceClosure acquired);

  // The IDs of `requests` that could not be had now.
  std::vector<IndexedDbLockId> GetUnacquirableLocks(
      const std::vector<LockRequest>& requests) const;
  // Whether a request waits for any of `held`.
  bool IsBlockingAnyRequest(const std::vector<IndexedDbLockId>& held) const;

 private:
  friend class IndexedDbLock;

  struct Waiting {
    LockType type = LockType::kShared;
    base::WeakPtr<IndexedDbLockHolder> holder;
    base::OnceClosure acquired;
  };
  struct Lock {
    Lock();
    Lock(Lock&&);
    Lock& operator=(Lock&&);
    ~Lock();

    bool CanBeAcquired(LockType type) const {
      return acquired_count == 0 ||
             (queue.empty() && mode == LockType::kShared &&
              type == LockType::kShared);
    }

    int acquired_count = 0;
    LockType mode = LockType::kShared;
    base::circular_deque<Waiting> queue;
  };

  void AcquireLock(LockRequest request,
                   IndexedDbLockHolder& holder,
                   base::OnceClosure acquired);
  void LockReleased(const IndexedDbLockId& id);

  std::map<IndexedDbLockId, Lock> locks_;
  base::WeakPtrFactory<IndexedDbLockManager> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_LOCK_MANAGER_H_
