#include "mitto/dom/body_loader_bytes_consumer.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

// What the Runtime's loaders say stands for both of what a data pipe's
// consumer hears: AppendBody() for the pipe becoming readable, and
// SignalComplete() for the end of the pipe and the loader's completion
// together, since the body's last bytes and its end come in that order.

BodyLoaderBytesConsumer::BodyLoaderBytesConsumer(BodyLoader* loader,
                                                 Window* window)
    : loader_(loader), window_(window) {}

BodyLoaderBytesConsumer::~BodyLoaderBytesConsumer() = default;

void BodyLoaderBytesConsumer::AppendBody(std::string_view bytes) {
  if (!IsWaiting() || completion_signaled_ || bytes.empty()) {
    return;
  }
  buffer_.append(bytes);
  Notify();
}

BytesConsumer::Result BodyLoaderBytesConsumer::BeginRead(
    base::span<const char>& buffer) {
  DCHECK(!is_in_two_phase_read_);
  buffer = {};
  if (state_ == InternalState::kClosed) {
    return Result::kDone;
  }
  if (state_ == InternalState::kErrored) {
    return Result::kError;
  }
  if (!requested_) {
    requested_ = true;
    if (loader_) {
      loader_->BodyRequested();
    }
  }
  if (Unread() > 0) {
    is_in_two_phase_read_ = true;
    buffer = base::span(buffer_).subspan(offset_);
    return Result::kOk;
  }
  // The end of the pipe, once its completion has come too.
  if (completion_signaled_) {
    MaybeClose();
    return IsWaiting() ? Result::kShouldWait : Result::kDone;
  }
  return Result::kShouldWait;
}

BytesConsumer::Result BodyLoaderBytesConsumer::EndRead(size_t read) {
  DCHECK(is_in_two_phase_read_);
  is_in_two_phase_read_ = false;
  DCHECK(IsWaiting());
  DCHECK_LE(read, Unread());
  offset_ += read;
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  }
  if (read > 0 && loader_) {
    loader_->BodyTaken(read);
  }
  if (has_pending_complete_) {
    has_pending_complete_ = false;
    SignalComplete();
    return Result::kOk;
  }
  if (has_pending_error_) {
    has_pending_error_ = false;
    SignalError();
    return Result::kError;
  }
  if (has_pending_notification_) {
    has_pending_notification_ = false;
    ScheduleNotify();
  }
  return Result::kOk;
}

void BodyLoaderBytesConsumer::SetClient(BytesConsumer::Client* client) {
  DCHECK(!client_);
  DCHECK(client);
  if (IsWaiting()) {
    client_ = client;
  }
}

void BodyLoaderBytesConsumer::ClearClient() {
  client_ = nullptr;
}

void BodyLoaderBytesConsumer::Cancel() {
  DCHECK(!is_in_two_phase_read_);
  ClearClient();
  buffer_.clear();
  offset_ = 0;
  // Closing the consumer end of the pipe: the loader drops the rest.
  if (loader_) {
    loader_->BodyDropped();
    loader_ = nullptr;
  }
  SignalComplete();
}

BytesConsumer::PublicState BodyLoaderBytesConsumer::GetPublicState() const {
  return GetPublicStateFromInternalState(state_);
}

void BodyLoaderBytesConsumer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
  visitor->Trace(client_);
  BytesConsumer::Trace(visitor);
}

void BodyLoaderBytesConsumer::MaybeClose() {
  DCHECK(!is_in_two_phase_read_);
  if (!completion_signaled_ || Unread() > 0 || !IsWaiting()) {
    return;
  }
  state_ = InternalState::kClosed;
  ClearClient();
}

void BodyLoaderBytesConsumer::SignalComplete() {
  if (!IsWaiting() || has_pending_complete_ || has_pending_error_) {
    return;
  }
  if (is_in_two_phase_read_) {
    has_pending_complete_ = true;
    return;
  }
  completion_signaled_ = true;
  BytesConsumer::Client* const client = client_.Get();
  MaybeClose();
  if (!IsWaiting()) {
    if (client) {
      client->OnStateChange();
    }
    return;
  }
  // There is still some of the body to read: the watcher sees that the pipe
  // is readable.
  ScheduleNotify();
}

void BodyLoaderBytesConsumer::SignalError() {
  if (!IsWaiting() || has_pending_complete_ || has_pending_error_) {
    return;
  }
  if (is_in_two_phase_read_) {
    has_pending_error_ = true;
    return;
  }
  BytesConsumer::Client* const client = client_.Get();
  // When we hit an error we switch states immediately. We don't wait for the
  // end of the pipe to be read.
  SetError();
  if (client) {
    client->OnStateChange();
  }
}

void BodyLoaderBytesConsumer::SignalErrorLater() {
  PostWindowTask(window_.Get(), TaskType::kNetworking,
                 base::BindOnce(
                     [](cppgc::WeakPersistent<BodyLoaderBytesConsumer> self) {
                       if (self) {
                         self->SignalError();
                       }
                     },
                     cppgc::WeakPersistent<BodyLoaderBytesConsumer>(this)));
}

void BodyLoaderBytesConsumer::SetError() {
  DCHECK(!is_in_two_phase_read_);
  if (!IsWaiting()) {
    return;
  }
  buffer_.clear();
  offset_ = 0;
  state_ = InternalState::kErrored;
  ClearClient();
}

void BodyLoaderBytesConsumer::Notify() {
  if (!IsWaiting()) {
    return;
  }
  // If the pipe signals us in the middle of our client reading, then delay
  // processing the signal until the read is complete.
  if (is_in_two_phase_read_) {
    has_pending_notification_ = true;
    return;
  }
  BytesConsumer::Client* const client = client_.Get();
  if (Unread() == 0) {
    if (!completion_signaled_) {
      // Not readable yet: keep waiting.
      return;
    }
    // We've reached the end of the pipe.
    MaybeClose();
    if (IsWaiting()) {
      return;
    }
  }
  if (client) {
    client->OnStateChange();
  }
}

void BodyLoaderBytesConsumer::ScheduleNotify() {
  // The watcher's task, on the networking task runner of the window.
  PostWindowTask(window_.Get(), TaskType::kNetworking,
                 base::BindOnce(
                     [](cppgc::WeakPersistent<BodyLoaderBytesConsumer> self) {
                       if (self) {
                         self->Notify();
                       }
                     },
                     cppgc::WeakPersistent<BodyLoaderBytesConsumer>(this)));
}

void BodyLoaderBytesConsumer::Dispose() {
  if (loader_) {
    loader_->BodyDropped();
    loader_ = nullptr;
  }
}

}  // namespace mitto
