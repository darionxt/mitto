#include "mitto/dom/blob_bytes_consumer.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/string_view_util.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// Blink's kBlobMinDataPipeCapacity and kBlobDefaultDataPipeCapacity.
constexpr size_t kMinDataPipeCapacity = 1024;
constexpr size_t kDefaultDataPipeCapacity = 2 * 1024 * 1024;

}  // namespace

BlobBytesConsumer::BlobBytesConsumer(ExecutionContext* context,
                                     scoped_refptr<BlobDataHandle> handle)
    : context_(context), handle_(std::move(handle)) {
  pipe_capacity_ = std::max(kMinDataPipeCapacity,
                            static_cast<size_t>(std::min<uint64_t>(
                                handle_->size(), kDefaultDataPipeCapacity)));
}

BlobBytesConsumer::~BlobBytesConsumer() = default;

BytesConsumer::Result BlobBytesConsumer::BeginRead(
    base::span<const char>& buffer) {
  buffer = {};
  if (state_ == PublicState::kClosed) {
    return Result::kDone;
  }
  if (!started_) {
    started_ = true;
    waiting_ = true;
    PostTaskInContext(
        context_.Get(), TaskType::kNetworking,
        base::BindOnce(
            [](cppgc::Persistent<BlobBytesConsumer> self) { self->FillPipe(); },
            cppgc::Persistent<BlobBytesConsumer>(this)));
    return Result::kShouldWait;
  }
  if (waiting_) {
    return Result::kShouldWait;
  }
  if (offset_ == handle_->size()) {
    state_ = PublicState::kClosed;
    handle_ = nullptr;
    return Result::kDone;
  }
  buffer =
      base::as_chars(handle_->Bytes().subspan(offset_, available_ - offset_));
  return Result::kOk;
}

BytesConsumer::Result BlobBytesConsumer::EndRead(size_t read_size) {
  DCHECK_LE(offset_ + read_size, available_);
  offset_ += read_size;
  if (offset_ == handle_->size()) {
    state_ = PublicState::kClosed;
    handle_ = nullptr;
    return Result::kDone;
  }
  if (offset_ == available_) {
    waiting_ = true;
    PostTaskInContext(
        context_.Get(), TaskType::kNetworking,
        base::BindOnce(
            [](cppgc::Persistent<BlobBytesConsumer> self) { self->FillPipe(); },
            cppgc::Persistent<BlobBytesConsumer>(this)));
  }
  return Result::kOk;
}

void BlobBytesConsumer::FillPipe() {
  if (state_ != PublicState::kReadableOrWaiting || !waiting_) {
    return;
  }
  waiting_ = false;
  available_ = static_cast<size_t>(
      std::min<uint64_t>(handle_->size(), offset_ + pipe_capacity_));
  if (client_) {
    client_->OnStateChange();
  }
}

std::optional<std::string> BlobBytesConsumer::DrainAsBytes() {
  if (started_ || state_ != PublicState::kReadableOrWaiting) {
    return std::nullopt;
  }
  std::string bytes(base::as_string_view(handle_->Bytes()));
  state_ = PublicState::kClosed;
  handle_ = nullptr;
  return bytes;
}

void BlobBytesConsumer::SetClient(BytesConsumer::Client* client) {
  DCHECK(!client_);
  DCHECK(client);
  client_ = client;
}

void BlobBytesConsumer::ClearClient() {
  client_ = nullptr;
}

void BlobBytesConsumer::Cancel() {
  state_ = PublicState::kClosed;
  handle_ = nullptr;
  client_ = nullptr;
}

BytesConsumer::PublicState BlobBytesConsumer::GetPublicState() const {
  return state_;
}

void BlobBytesConsumer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(context_);
  visitor->Trace(client_);
  BytesConsumer::Trace(visitor);
}

}  // namespace mitto
