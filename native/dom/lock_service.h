// The Web Locks of the Page's documents, as the browser's LockManager keeps
// them for each origin's lock group: for each name, a queue of requests whose
// granted ones come first, shared ones granted together and an exclusive one
// alone. Chrome keeps one group for an origin across a profile's tabs and
// workers; the Page keeps one for each origin across its own documents, which
// are all the clients it has.
//
// It stands where the browser does: what a client asks reaches it from a task
// of its own, and what it tells a client goes back from another, as a message
// each way over mojo would.
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
#include "base/memory/ref_counted.h"

namespace mitto {

class LockService {
 public:
  enum class Mode { kShared, kExclusive };
  // blink.mojom.LockManager's WaitMode: wait for the lock, fail at once if it
  // is held, or break the locks that hold it.
  enum class WaitMode { kWait, kNoWait, kPreempt };

  // A client's end: what the service tells it about each of its requests,
  // each from a task of its own. A lock is the request's, by its ID.
  class Client : public base::RefCounted<Client> {
   public:
    // The request now holds its lock.
    virtual void Granted(int64_t lock_id) = 0;
    // The request asked not to wait, and the lock is held.
    virtual void Failed(int64_t lock_id) = 0;
    // A request that steals the lock broke the one this held.
    virtual void Broken(int64_t lock_id) = 0;

   protected:
    friend class base::RefCounted<Client>;
    virtual ~Client() = default;
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
  // lock group of `origin`. Its answer comes once it has reached the service.
  int64_t RequestLock(scoped_refptr<Client> client,
                      std::string origin,
                      std::string client_id,
                      std::string name,
                      Mode mode,
                      WaitMode wait);
  // The client let go of the request or the lock of `lock_id`, as when its end
  // of the pipe closes.
  void ReleaseLock(std::string origin, int64_t lock_id);
  // The held locks and the waiting requests of the lock group of `origin`, as
  // they stand once the question has reached the service, by name. `answer`
  // gets them from a task of its own.
  void QueryState(std::string origin,
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
  void DoRequestLock(std::string origin, Lock lock, WaitMode wait);
  void DoReleaseLock(std::string origin, int64_t lock_id);
  std::pair<std::vector<Info>, std::vector<Info>> Snapshot(
      const std::string& origin) const;

  std::map<std::string, std::unique_ptr<LockGroupState>> lock_groups_;
  int64_t next_lock_id_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCK_SERVICE_H_
