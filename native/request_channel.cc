#include "mitto/request_channel.h"

#include <algorithm>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "mitto/mitto.h"
#include "net/base/net_errors.h"

namespace mitto {

RequestEvent::RequestEvent() = default;
RequestEvent::RequestEvent(RequestEvent&&) = default;
RequestEvent& RequestEvent::operator=(RequestEvent&&) = default;
RequestEvent::~RequestEvent() = default;

RequestChannel::RequestChannel()
    : event_waiting_(&lock_),
      read_buffer_(
          base::MakeRefCounted<net::IOBufferWithSize>(kReadBufferSize)) {}

RequestChannel::~RequestChannel() = default;

int RequestChannel::NextEvent(std::optional<base::TimeDelta> timeout,
                              RequestEvent& event) {
  base::AutoLock lock(lock_);
  const base::TimeTicks deadline =
      timeout ? base::TimeTicks::Now() + *timeout : base::TimeTicks::Max();
  // No event comes after the last one, so that one ends the wait as well.
  while (events_.empty() && !finished_) {
    if (!timeout) {
      event_waiting_.Wait();
      continue;
    }
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      return net::ERR_IO_PENDING;
    }
    event_waiting_.TimedWait(remaining);
  }
  if (events_.empty()) {
    return net::ERR_INVALID_ARGUMENT;
  }
  event = std::move(events_.front());
  events_.pop_front();
  return net::OK;
}

int RequestChannel::Read(base::span<uint8_t> output, bool& start_reading) {
  start_reading = false;
  base::AutoLock lock(lock_);
  if (unread_size_ > 0) {
    const size_t size = std::min(output.size(), unread_size_);
    output.first(size).copy_from(
        read_buffer_->span().subspan(unread_offset_, size));
    unread_offset_ += size;
    unread_size_ -= size;
    return static_cast<int>(size);
  }
  if (finished_) {
    return 0;
  }
  start_reading = !read_requested_;
  read_requested_ = true;
  return net::ERR_IO_PENDING;
}

void RequestChannel::Push(RequestEvent event) {
  base::AutoLock lock(lock_);
  PushLocked(std::move(event));
}

void RequestChannel::BodyArrived(size_t size) {
  RequestEvent event;
  event.type = MITTO_EVENT_BODY_AVAILABLE;
  base::AutoLock lock(lock_);
  unread_offset_ = 0;
  unread_size_ = size;
  read_requested_ = false;
  PushLocked(std::move(event));
}

void RequestChannel::Finish(int net_error) {
  RequestEvent event;
  event.type = MITTO_EVENT_FINISHED;
  event.net_error = net_error;
  base::AutoLock lock(lock_);
  finished_ = true;
  PushLocked(std::move(event));
}

void RequestChannel::PushLocked(RequestEvent event) {
  events_.push_back(std::move(event));
  event_waiting_.Broadcast();
}

}  // namespace mitto
