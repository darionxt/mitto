// The Web Locks of the Page's documents and workers, as the browser's
// LockManager keeps them for each lock group, a storage key's: for each name, a
// queue of requests whose granted ones come first, shared ones granted together
// and an exclusive one alone. Chrome keeps one group for a storage key across a
// profile's tabs and workers; the Page keeps one for each across its own
// documents and workers, which are all the clients it has.
//
// It stands where the browser does: what a client asks reaches it from a task
// of its own, and what it tells a client goes back from another, on the
// client's thread, as a message each way over mojo would. Its state is held
// under a lock, as clients on each thread reach it.
//
// Upstream: content/browser/locks/lock_manager.h

#ifndef MITTO_DOM_LOCK_SERVICE_H_
#define MITTO_DOM_LOCK_SERVICE_H_

#include <stdint.h>

#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"

namespace mitto {

class LockService {
 public:
  enum class Mode { kShared, kExclusive };
  // blink.mojom.LockManager's WaitMode: wait for the lock, fail at once if it
  // is held, or break the locks that hold it.
  enum class WaitMode { kWait, kNoWait, kPreempt };

  // A client's end: what the service tells it about each of its requests,
  // each from a task of its own on `runner`, its thread's queue of Web Locks,
  // where it is also deleted. A lock is the request's, by its ID.
  class Client : public base::RefCountedDeleteOnSequence<Client> {
   public:
    explicit Client(scoped_refptr<base::SequencedTaskRunner> runner);

    // Runs `task` on the client's thread, from a task of its own.
    void PostToClient(base::OnceClosure task);

    // The request now holds its lock.
    virtual void Granted(int64_t lock_id) = 0;
    // The request asked not to wait, and the lock is held.
    virtual void Failed(int64_t lock_id) = 0;
    // A request that steals the lock broke the one this held.
    virtual void Broken(int64_t lock_id) = 0;

   protected:
    friend class base::RefCountedDeleteOnSequence<Client>;
    friend class base::DeleteHelper<Client>;
    virtual ~Client();
  };

  // What query() gives of a lock or a request.
  struct Info {
    std::string name;
    Mode mode;
    std::string client_id;
  };

  // The Page's.
  static LockService& Get();

  LockService();
  LockService(const LockService&) = delete;
  LockService& operator=(const LockService&) = delete;
  ~LockService();

  // The ID of a new lock request of `client`, whose ID is `client_id`, in the
  // lock group `group`. Its answer comes once it has reached the service.
  int64_t RequestLock(scoped_refptr<Client> client,
                      std::string group,
                      std::string client_id,
                      std::string name,
                      Mode mode,
                      WaitMode wait);
  // The client let go of the request or the lock of `lock_id`, as when its end
  // of the pipe closes, at once: the client calls it from a task of its own,
  // after what it asked before, or as its context goes, whose thread may run
  // no task again, as a worker's that ends.
  void ReleaseLockNow(const std::string& group, int64_t lock_id);
  // The held locks and the waiting requests of the lock group `group`, as
  // they stand once the question has reached the service, by name. `answer`
  // gets them from a task of its own.
  void QueryState(std::string group,
                  base::OnceCallback<void(std::vector<Info> pending,
                                          std::vector<Info> held)> answer);

 private:
  // A request, and once it is granted, its lock.
  struct Lock {
    Lock(scoped_refptr<Client> client,
         std::string client_id,
         std::string name,
         Mode mode,
         int64_t lock_id);
    Lock(Lock&&);
    Lock& operator=(Lock&&);
    ~Lock();

    scoped_refptr<Client> client;
    std::string client_id;
    std::string name;
    Mode mode;
    int64_t lock_id;
    bool granted = false;
  };
  class LockGroupState;

  // Each runs in a task of its own, as a message reaches the browser.
  void DoRequestLock(std::string group, Lock lock, WaitMode wait);
  void DoReleaseLock(std::string group, int64_t lock_id);
  std::pair<std::vector<Info>, std::vector<Info>> Snapshot(
      const std::string& group) const;

  mutable base::Lock lock_;
  std::map<std::string, std::unique_ptr<LockGroupState>> lock_groups_
      GUARDED_BY(lock_);
  int64_t next_lock_id_ GUARDED_BY(lock_) = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCK_SERVICE_H_
