#include "mitto/dom/bytes_consumer.h"

#include "base/notreached.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/blob_data.h"

namespace mitto {
namespace {

class ErroredBytesConsumer final : public BytesConsumer {
 public:
  Result BeginRead(base::span<const char>& buffer) override {
    buffer = {};
    return Result::kError;
  }
  Result EndRead(size_t read_size) override { NOTREACHED(); }
  void SetClient(BytesConsumer::Client* client) override {}
  void ClearClient() override {}
  void Cancel() override {}
  PublicState GetPublicState() const override { return PublicState::kErrored; }
};

class ClosedBytesConsumer final : public BytesConsumer {
 public:
  Result BeginRead(base::span<const char>& buffer) override {
    buffer = {};
    return Result::kDone;
  }
  Result EndRead(size_t read_size) override { NOTREACHED(); }
  void SetClient(BytesConsumer::Client* client) override {}
  void ClearClient() override {}
  void Cancel() override {}
  PublicState GetPublicState() const override { return PublicState::kClosed; }
};

}  // namespace

std::optional<std::string> BytesConsumer::DrainAsBytes() {
  return std::nullopt;
}

scoped_refptr<BlobDataHandle> BytesConsumer::DrainAsBlobDataHandle() {
  return nullptr;
}

// static
BytesConsumer* BytesConsumer::CreateErrored() {
  return bindings::MakeGarbageCollected<ErroredBytesConsumer>();
}

// static
BytesConsumer* BytesConsumer::CreateClosed() {
  return bindings::MakeGarbageCollected<ClosedBytesConsumer>();
}

// static
BytesConsumer::PublicState BytesConsumer::GetPublicStateFromInternalState(
    InternalState state) {
  switch (state) {
    case InternalState::kWaiting:
      return PublicState::kReadableOrWaiting;
    case InternalState::kClosed:
      return PublicState::kClosed;
    case InternalState::kErrored:
      return PublicState::kErrored;
  }
  NOTREACHED();
}

}  // namespace mitto
