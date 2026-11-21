// What the threads of a Page send to its Session: the main thread and each
// dedicated worker's thread. The I/O thread sends frames in the order they
// reach it, and the Session holds the numbers of requests and the IDs of
// contexts to be larger than those before them, so this takes each such
// number under one lock with the sending of the frame that carries it, and two
// threads' frames reach the Session in the order of their numbers. It also
// keeps where the response of each request of a worker goes, for the I/O
// thread to hand it to that worker's thread, and which contexts the Session
// has forgotten: under the same lock, nothing that such a context sends goes
// after the frame that made the Session forget it, as Chrome's browser drops
// what the pipes of a context that went still carry.

#ifndef MITTO_PAGE_SESSION_SENDER_H_
#define MITTO_PAGE_SESSION_SENDER_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionSender final : public base::RefCountedThreadSafe<SessionSender> {
 public:
  // Takes a frame of the response of a request, a response started, part of
  // a body or the end, on the I/O thread.
  using ResponseRoute = base::RepeatingCallback<void(FrameFromSession frame)>;

  // `send` hands a frame to the I/O thread, from any thread.
  explicit SessionSender(base::RepeatingCallback<void(std::string)> send);

  SessionSender(const SessionSender&) = delete;
  SessionSender& operator=(const SessionSender&) = delete;

  // Sends `frame`, which names no context. Any thread.
  void Send(std::string frame);

  // Sends `frame`, which `context` sends, unless the Session has forgotten
  // `context`. Any thread.
  void SendFromContext(uint64_t context, std::string frame);

  // Takes the number of the next request, of `context`, sends the frame that
  // `encode` makes of it, and returns it. Without a route, its response goes
  // to the main thread; with one, as for a worker's request, to `route` until
  // its end. A request of a context that the Session has forgotten takes its
  // number and goes nowhere. Any thread.
  uint64_t SendRequest(uint64_t context,
                       base::FunctionRef<std::string(uint64_t number)> encode,
                       ResponseRoute route = ResponseRoute());

  // Sends `frame`, a cancel of request `number`, credit for its body or part
  // of the body it sends, unless the request went nowhere. Any thread.
  void SendForRequest(uint64_t number, std::string frame);

  // Takes the ID of the next context, a top-level document, sends the frame
  // that `encode` makes of it, and returns it. It takes the place of every
  // context before it, which the Session forgets. Any thread.
  uint64_t SendTopLevelDocument(
      base::FunctionRef<std::string(uint64_t id)> encode);

  // Takes the ID of the next context, a worker that `parent` started, sends
  // the frame that `encode` makes of it, and returns it. One under a context
  // that the Session has forgotten goes unannounced, and is forgotten too.
  // Any thread.
  uint64_t SendContext(uint64_t parent,
                       base::FunctionRef<std::string(uint64_t id)> encode);

  // Says that `context` has gone, unless the Session has forgotten it, and
  // forgets it and the contexts under it, which the Session forgets with it.
  // Any thread.
  void SendContextGone(uint64_t context);

  // Hands `frame`, a frame of a response or credit for a request body, to the
  // route of its request, and forgets the route once the request ends.
  // Returns false for a request that has none, whose response goes to the
  // main thread. The I/O thread.
  bool Route(FrameFromSession& frame);

 private:
  friend class base::RefCountedThreadSafe<SessionSender>;
  ~SessionSender();

  // Forgets `context` and the contexts under it.
  void ForgetUnder(uint64_t context) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const base::RepeatingCallback<void(std::string)> send_;
  base::Lock lock_;
  uint64_t last_request_ GUARDED_BY(lock_) = 0;
  uint64_t last_context_ GUARDED_BY(lock_) = kNoDocument;
  std::map<uint64_t, ResponseRoute> routes_ GUARDED_BY(lock_);
  // The parent of each context that the Session knows, kNoDocument for the
  // top-level document. Each comes after its parent.
  std::map<uint64_t, uint64_t> parents_ GUARDED_BY(lock_);
  // The contexts that the Session has forgotten, and the requests of theirs
  // that went nowhere.
  std::set<uint64_t> forgotten_ GUARDED_BY(lock_);
  std::set<uint64_t> unsent_requests_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_SENDER_H_
