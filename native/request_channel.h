#ifndef MITTO_REQUEST_CHANNEL_H_
#define MITTO_REQUEST_CHANNEL_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>

#include "base/containers/circular_deque.h"
#include "base/containers/span.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "net/base/io_buffer.h"

namespace mitto {

// One event of a request. Each MITTO_EVENT type uses the fields that MittoEvent
// says it does.
struct RequestEvent {
  RequestEvent();
  RequestEvent(RequestEvent&&);
  RequestEvent& operator=(RequestEvent&&);
  ~RequestEvent();

  int32_t type = 0;
  int32_t status = 0;
  int32_t net_error = 0;
  std::string protocol;
  std::string url;
  std::string headers;
};

// Carries one request's events and body from the network thread to whichever
// thread holds its handle. The network thread only ever holds the lock for a
// moment, so a caller that is slow or blocked cannot hold up its traffic.
class RequestChannel : public base::RefCountedThreadSafe<RequestChannel> {
 public:
  // How much of a body is held here at most. The network thread reads into a
  // buffer of this size, and only once the caller has emptied it and asked for
  // more.
  static constexpr int kReadBufferSize = 64 * 1024;

  RequestChannel();

  RequestChannel(const RequestChannel&) = delete;
  RequestChannel& operator=(const RequestChannel&) = delete;

  // Called on any thread.
  //
  // Takes the next event into `event` and returns net::OK. Waits for one for up
  // to `timeout`, or for as long as it takes if that is nullopt. Returns
  // net::ERR_IO_PENDING if none arrived in that time, and
  // net::ERR_INVALID_ARGUMENT if the last event was already taken.
  int NextEvent(std::optional<base::TimeDelta> timeout, RequestEvent& event);

  // Called on any thread.
  //
  // Copies body bytes that have arrived into `output` and returns how many. If
  // none are waiting, returns 0 once the request has finished and otherwise
  // net::ERR_IO_PENDING. In that case `start_reading` says whether the caller
  // has to make the network thread read, because it is not doing so already.
  int Read(base::span<uint8_t> output, bool& start_reading);

  // The rest is called on the network thread.

  // What to read the body into. Nothing else touches it during a read, because
  // a read only starts once the buffer is empty.
  net::IOBuffer* read_buffer() const { return read_buffer_.get(); }

  void Push(RequestEvent event);

  // Makes the first `size` bytes of the buffer available to Read(), and pushes
  // the event that says so.
  void BodyArrived(size_t size);

  // Pushes the last event, with `net_error`. Bytes that have not been read stay
  // available to Read().
  void Finish(int net_error);

 private:
  friend class base::RefCountedThreadSafe<RequestChannel>;

  ~RequestChannel();

  void PushLocked(RequestEvent event) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;
  base::ConditionVariable event_waiting_;
  base::circular_deque<RequestEvent> events_ GUARDED_BY(lock_);
  const scoped_refptr<net::IOBufferWithSize> read_buffer_;
  size_t unread_offset_ GUARDED_BY(lock_) = 0;
  size_t unread_size_ GUARDED_BY(lock_) = 0;
  bool read_requested_ GUARDED_BY(lock_) = false;
  bool finished_ GUARDED_BY(lock_) = false;
};

}  // namespace mitto

#endif  // MITTO_REQUEST_CHANNEL_H_
