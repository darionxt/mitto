#ifndef MITTO_READY_REQUESTS_H_
#define MITTO_READY_REQUESTS_H_

#include <stdint.h>

#include <optional>

#include "base/containers/circular_deque.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include "base/files/scoped_file.h"
#endif

namespace mitto {

// Which of a Session's requests have an event waiting, and two ways to hear of
// them without asking each request: a file descriptor that is readable while
// any do, and Wait(), for a thread of its own. One list serves all of a
// Session's requests, because a pipe per request would use up the limit on open
// files at a few hundred requests, most of which would only be waiting their
// turn for a connection. Windows has no such descriptor, so Wait() is the only
// way there.
//
// A request is named by the address of its handle and never dereferenced here,
// because the handle may be gone by the time its name is taken.
//
// Nothing is listed until the first call to GetFileDescriptor() or Wait(), so
// that a caller who uses neither pays nothing and has nothing pile up.
class ReadyRequests {
 public:
  ReadyRequests();

  ReadyRequests(const ReadyRequests&) = delete;
  ReadyRequests& operator=(const ReadyRequests&) = delete;

  ~ReadyRequests();

#if BUILDFLAG(IS_POSIX)
  // Called on any thread. Returns nullopt if there is no room for a pipe.
  std::optional<int> GetFileDescriptor();
#endif

  // Called on any thread.
  //
  // Waits until a request is listed that this has not returned for, and returns
  // net::OK. It returns once each time the list goes from empty to holding a
  // request: after it has returned, it waits until Take() has emptied the list
  // and another request is listed. So a thread that hands each return to a
  // caller who has not caught up yet sleeps until the caller has.
  //
  // Waits for up to `timeout`, or for as long as it takes if that is nullopt.
  // Returns net::ERR_IO_PENDING if nothing came in that time, and
  // net::ERR_ABORTED at once after EndWaits().
  int Wait(std::optional<base::TimeDelta> timeout);

  // Called on any thread. Ends each Wait() that waits, and each later one,
  // without waiting for them.
  void EndWaits();

  // Called on any thread, by what destroys the Session. Does as EndWaits() does,
  // and returns once no Wait() runs, so that the list can go.
  void EndWaitsAndWaitForThem();

  // Called on any thread. Returns nullopt if nothing is listed.
  std::optional<uintptr_t> Take();

  // Called on the network thread, once for each event that a request gets. It
  // does not wait for anything: the pipe holds one byte at most.
  void Add(uintptr_t request);

 private:
  base::Lock lock_;
  // Signalled when a waiting Wait() may have to return, and when the last one
  // returns after EndWaits().
  base::ConditionVariable changed_;
  bool listing_ GUARDED_BY(lock_) = false;
  base::circular_deque<uintptr_t> requests_ GUARDED_BY(lock_);
  // Wait() has returned for what is listed now.
  bool returned_for_list_ GUARDED_BY(lock_) = false;
  bool waits_ended_ GUARDED_BY(lock_) = false;
  int running_waits_ GUARDED_BY(lock_) = 0;
#if BUILDFLAG(IS_POSIX)
  base::ScopedFD read_end_ GUARDED_BY(lock_);
  base::ScopedFD write_end_ GUARDED_BY(lock_);
#endif
};

}  // namespace mitto

#endif  // MITTO_READY_REQUESTS_H_
