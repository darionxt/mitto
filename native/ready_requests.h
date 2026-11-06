#ifndef MITTO_READY_REQUESTS_H_
#define MITTO_READY_REQUESTS_H_

#include <stdint.h>

#include <optional>

#include "base/containers/circular_deque.h"
#include "base/files/scoped_file.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace mitto {

// Which of a Session's requests have an event waiting, and a file descriptor
// that is readable while any do. One descriptor serves all of a Session's
// requests, because a pipe per request would use up the limit on open files at
// a few hundred requests, most of which would only be waiting their turn for a
// connection.
//
// A request is named by the address of its handle and never dereferenced here,
// because the handle may be gone by the time its name is taken.
//
// Nothing is listed until the first call to GetFileDescriptor(), so that a
// caller who does not use one pays nothing and has nothing pile up.
class ReadyRequests {
 public:
  ReadyRequests();

  ReadyRequests(const ReadyRequests&) = delete;
  ReadyRequests& operator=(const ReadyRequests&) = delete;

  ~ReadyRequests();

  // Called on any thread. Returns nullopt if there is no room for a pipe.
  std::optional<int> GetFileDescriptor();

  // Called on any thread. Returns nullopt if nothing is listed.
  std::optional<uintptr_t> Take();

  // Called on the network thread, once for each event that a request gets. It
  // does not wait for anything: the pipe holds one byte at most.
  void Add(uintptr_t request);

 private:
  base::Lock lock_;
  base::circular_deque<uintptr_t> requests_ GUARDED_BY(lock_);
  base::ScopedFD read_end_ GUARDED_BY(lock_);
  base::ScopedFD write_end_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_READY_REQUESTS_H_
