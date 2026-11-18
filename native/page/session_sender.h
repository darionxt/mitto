// What the threads of a Page send to its Session: the main thread and each
// dedicated worker's thread. The I/O thread sends frames in the order they
// reach it, and the Session holds the numbers of requests and the IDs of
// contexts to be larger than those before them, so this takes each such
// number under one lock with the sending of the frame that carries it, and two
// threads' frames reach the Session in the order of their numbers. It also
// keeps where the response of each request of a worker goes, for the I/O
// thread to hand it to that worker's thread.

#ifndef MITTO_PAGE_SESSION_SENDER_H_
#define MITTO_PAGE_SESSION_SENDER_H_

#include <stdint.h>

#include <map>
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

  // Sends `frame`. Any thread.
  void Send(std::string frame);

  // Takes the number of the next request, sends the frame that `encode` makes
  // of it, and returns it. Without a route, its response goes to the main
  // thread; with one, as for a worker's request, to `route` until its end.
  // Any thread.
  uint64_t SendRequest(base::FunctionRef<std::string(uint64_t number)> encode,
                       ResponseRoute route = ResponseRoute());

  // Takes the ID of the next context, a document or a worker, sends the frame
  // that `encode` makes of it, and returns it. Any thread.
  uint64_t SendContext(base::FunctionRef<std::string(uint64_t id)> encode);

  // Hands `frame`, a frame of a response, to the route of its request, and
  // forgets the route once the request ends. Returns false for a request that
  // has none, whose response goes to the main thread. The I/O thread.
  bool Route(FrameFromSession& frame);

 private:
  friend class base::RefCountedThreadSafe<SessionSender>;
  ~SessionSender();

  const base::RepeatingCallback<void(std::string)> send_;
  base::Lock lock_;
  uint64_t last_request_ GUARDED_BY(lock_) = 0;
  uint64_t last_context_ GUARDED_BY(lock_) = kNoDocument;
  std::map<uint64_t, ResponseRoute> routes_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_SENDER_H_
