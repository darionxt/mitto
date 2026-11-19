#include "mitto/dom/buffering_bytes_consumer.h"

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/event_loop.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

constexpr base::TimeDelta kDelay = base::Milliseconds(50);

}  // namespace

// static
BufferingBytesConsumer* BufferingBytesConsumer::CreateWithDelay(
    BytesConsumer* bytes_consumer) {
  auto* const consumer =
      bindings::MakeGarbageCollected<BufferingBytesConsumer>(bytes_consumer);
  // Blink's timer, on the networking task runner, which holds it weakly.
  EventLoop::Get()
      ->TaskRunner(TaskType::kNetworking)
      ->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](cppgc::WeakPersistent<BufferingBytesConsumer> consumer) {
                if (consumer) {
                  consumer->OnTimerFired();
                }
              },
              cppgc::WeakPersistent<BufferingBytesConsumer>(consumer)),
          kDelay);
  return consumer;
}

BufferingBytesConsumer::BufferingBytesConsumer(BytesConsumer* bytes_consumer)
    : bytes_consumer_(bytes_consumer) {
  bytes_consumer_->SetClient(this);
}

BufferingBytesConsumer::~BufferingBytesConsumer() = default;

void BufferingBytesConsumer::MaybeStartBuffering() {
  if (buffering_state_ != BufferingState::kDelayed) {
    return;
  }
  buffering_state_ = BufferingState::kStarted;
  BufferData();
}

BytesConsumer::Result BufferingBytesConsumer::BeginRead(
    base::span<const char>& buffer) {
  // Stop delaying buffering on the first read as it will no longer be safe to
  // drain the underlying |bytes_consumer_| anyway.
  MaybeStartBuffering();
  if (buffer_.empty()) {
    if (buffering_state_ != BufferingState::kStarted) {
      return bytes_consumer_->BeginRead(buffer);
    }
    if (has_seen_error_) {
      return Result::kError;
    }
    if (has_seen_end_of_data_) {
      ClearClient();
      return Result::kDone;
    }
    BufferData();
    if (has_seen_error_) {
      return Result::kError;
    }
    if (buffer_.empty()) {
      return has_seen_end_of_data_ ? Result::kDone : Result::kShouldWait;
    }
  }
  const std::string& first_chunk = buffer_.front();
  DCHECK_LT(offset_for_first_chunk_, first_chunk.size());
  buffer = base::span(first_chunk).subspan(offset_for_first_chunk_);
  return Result::kOk;
}

BytesConsumer::Result BufferingBytesConsumer::EndRead(size_t read_size) {
  if (buffer_.empty()) {
    if (buffering_state_ != BufferingState::kStarted) {
      return bytes_consumer_->EndRead(read_size);
    }
    DCHECK(has_seen_error_);
    return Result::kError;
  }
  const size_t first_chunk_size = buffer_.front().size();
  DCHECK_LE(offset_for_first_chunk_ + read_size, first_chunk_size);
  offset_for_first_chunk_ += read_size;
  if (offset_for_first_chunk_ == first_chunk_size) {
    const bool was_waiting_for_capacity =
        !has_seen_end_of_data_ && total_buffer_size_ >= kMaxBufferSize;
    total_buffer_size_ -= first_chunk_size;
    offset_for_first_chunk_ = 0;
    buffer_.pop_front();
    if (was_waiting_for_capacity && total_buffer_size_ < kMaxBufferSize) {
      // We might have stopped buffering due to not having enough space, so try
      // reading more.
      BufferData();
      if (has_seen_error_) {
        DCHECK(buffer_.empty());
        return Result::kError;
      }
    }
  }
  if (buffer_.empty() && has_seen_end_of_data_) {
    ClearClient();
    return Result::kDone;
  }
  return Result::kOk;
}

std::optional<std::string> BufferingBytesConsumer::DrainAsBytes() {
  return bytes_consumer_->DrainAsBytes();
}

void BufferingBytesConsumer::SetClient(BytesConsumer::Client* client) {
  client_ = client;
}

void BufferingBytesConsumer::ClearClient() {
  client_ = nullptr;
}

void BufferingBytesConsumer::Cancel() {
  ClearClient();
  bytes_consumer_->Cancel();
}

BytesConsumer::PublicState BufferingBytesConsumer::GetPublicState() const {
  if (buffer_.empty()) {
    return bytes_consumer_->GetPublicState();
  }
  return PublicState::kReadableOrWaiting;
}

void BufferingBytesConsumer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(bytes_consumer_);
  visitor->Trace(client_);
  BytesConsumer::Trace(visitor);
  BytesConsumer::Client::Trace(visitor);
}

void BufferingBytesConsumer::OnTimerFired() {
  MaybeStartBuffering();
}

void BufferingBytesConsumer::OnStateChange() {
  BytesConsumer::Client* const client = client_.Get();
  BufferData();
  if (client) {
    client->OnStateChange();
  }
}

void BufferingBytesConsumer::BufferData() {
  if (buffering_state_ != BufferingState::kStarted) {
    return;
  }
  DCHECK(bytes_consumer_);
  while (total_buffer_size_ < kMaxBufferSize) {
    base::span<const char> p;
    auto result = bytes_consumer_->BeginRead(p);
    if (result == Result::kShouldWait) {
      return;
    }
    if (result == Result::kOk) {
      buffer_.emplace_back(p.begin(), p.end());
      total_buffer_size_ += p.size();
      result = bytes_consumer_->EndRead(p.size());
    }
    if (result == Result::kDone) {
      has_seen_end_of_data_ = true;
      ClearClient();
      return;
    }
    if (result != Result::kOk) {
      buffer_.clear();
      total_buffer_size_ = 0;
      has_seen_error_ = true;
      ClearClient();
      return;
    }
  }
}

}  // namespace mitto
