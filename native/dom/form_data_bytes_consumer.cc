#include "mitto/dom/form_data_bytes_consumer.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/dom/blob_data.h"

namespace mitto {

// Blink's DataOnlyBytesConsumer.
FormDataBytesConsumer::FormDataBytesConsumer(std::string bytes)
    : bytes_(std::move(bytes)) {}

FormDataBytesConsumer::~FormDataBytesConsumer() = default;

BytesConsumer::Result FormDataBytesConsumer::BeginRead(
    base::span<const char>& buffer) {
  buffer = {};
  flattened_ = true;
  // An empty body gives kDone without closing, as in Blink.
  if (offset_ == bytes_.size()) {
    return Result::kDone;
  }
  buffer = base::span(bytes_).subspan(offset_);
  return Result::kOk;
}

BytesConsumer::Result FormDataBytesConsumer::EndRead(size_t read_size) {
  DCHECK_LE(offset_ + read_size, bytes_.size());
  offset_ += read_size;
  if (offset_ == bytes_.size()) {
    state_ = PublicState::kClosed;
    return Result::kDone;
  }
  return Result::kOk;
}

std::optional<std::string> FormDataBytesConsumer::DrainAsBytes() {
  if (flattened_ || state_ != PublicState::kReadableOrWaiting) {
    return std::nullopt;
  }
  state_ = PublicState::kClosed;
  flattened_ = true;
  std::string bytes;
  bytes.swap(bytes_);
  return bytes;
}

scoped_refptr<BlobDataHandle> FormDataBytesConsumer::DrainAsBlobDataHandle() {
  if (flattened_ || state_ != PublicState::kReadableOrWaiting) {
    return nullptr;
  }
  state_ = PublicState::kClosed;
  flattened_ = true;
  std::string bytes;
  bytes.swap(bytes_);
  return BlobDataHandle::Create(std::move(bytes), std::string());
}

void FormDataBytesConsumer::SetClient(BytesConsumer::Client* client) {
  DCHECK(client);
}

void FormDataBytesConsumer::ClearClient() {}

void FormDataBytesConsumer::Cancel() {
  state_ = PublicState::kClosed;
  flattened_ = true;
  bytes_.clear();
  offset_ = 0;
}

BytesConsumer::PublicState FormDataBytesConsumer::GetPublicState() const {
  return state_;
}

}  // namespace mitto
