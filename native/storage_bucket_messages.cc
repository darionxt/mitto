#include "mitto/storage_bucket_messages.h"

#include <utility>

#include "mitto/message_coding.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// The flags of kOpen's policies.
constexpr uint8_t kHasPersisted = 1;
constexpr uint8_t kPersisted = 2;
constexpr uint8_t kHasQuota = 4;
constexpr uint8_t kHasDurability = 8;
constexpr uint8_t kStrict = 16;
constexpr uint8_t kHasExpires = 32;
constexpr uint8_t kAllFlags = 63;

bool NamesBucket(StorageBucketCallOp op) {
  switch (op) {
    case StorageBucketCallOp::kPersist:
    case StorageBucketCallOp::kPersisted:
    case StorageBucketCallOp::kEstimate:
    case StorageBucketCallOp::kDurability:
    case StorageBucketCallOp::kSetExpires:
    case StorageBucketCallOp::kExpires:
      return true;
    default:
      return false;
  }
}

}  // namespace

PageStorageBucketCall::PageStorageBucketCall() = default;
PageStorageBucketCall::PageStorageBucketCall(const PageStorageBucketCall&) =
    default;
PageStorageBucketCall::PageStorageBucketCall(PageStorageBucketCall&&) = default;
PageStorageBucketCall& PageStorageBucketCall::operator=(
    const PageStorageBucketCall&) = default;
PageStorageBucketCall& PageStorageBucketCall::operator=(
    PageStorageBucketCall&&) = default;
PageStorageBucketCall::~PageStorageBucketCall() = default;

PageStorageBucketAnswer::PageStorageBucketAnswer() = default;
PageStorageBucketAnswer::PageStorageBucketAnswer(
    const PageStorageBucketAnswer&) = default;
PageStorageBucketAnswer::PageStorageBucketAnswer(PageStorageBucketAnswer&&) =
    default;
PageStorageBucketAnswer& PageStorageBucketAnswer::operator=(
    const PageStorageBucketAnswer&) = default;
PageStorageBucketAnswer& PageStorageBucketAnswer::operator=(
    PageStorageBucketAnswer&&) = default;
PageStorageBucketAnswer::~PageStorageBucketAnswer() = default;

std::string EncodeStorageBucketCall(const PageStorageBucketCall& call) {
  MessageWriter writer;
  writer.Byte(static_cast<uint8_t>(call.op));
  if (call.op == StorageBucketCallOp::kRelease) {
    writer.U64(call.bucket);
    return writer.Take();
  }
  writer.U64(call.question);
  if (NamesBucket(call.op)) {
    writer.U64(call.bucket);
  }
  switch (call.op) {
    case StorageBucketCallOp::kOpen: {
      writer.Sized(call.name);
      uint8_t flags = 0;
      if (call.persisted) {
        flags |= kHasPersisted | (*call.persisted ? kPersisted : 0);
      }
      if (call.quota) {
        flags |= kHasQuota;
      }
      if (call.durability) {
        flags |= kHasDurability | (*call.durability == 1 ? kStrict : 0);
      }
      if (call.expires) {
        flags |= kHasExpires;
      }
      writer.Byte(flags);
      if (call.quota) {
        writer.U64(*call.quota);
      }
      if (call.expires) {
        writer.I64(*call.expires);
      }
      break;
    }
    case StorageBucketCallOp::kDelete:
      writer.Sized(call.name);
      break;
    case StorageBucketCallOp::kSetExpires:
      writer.I64(call.expires.value_or(0));
      break;
    default:
      break;
  }
  return writer.Take();
}

base::expected<PageStorageBucketCall, std::string> ReadStorageBucketCall(
    std::string_view payload) {
  MessageReader reader(payload);
  PageStorageBucketCall call;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(StorageBucketCallOp::kMaxValue)) {
    return base::unexpected("a Storage Buckets call of an unknown operation");
  }
  call.op = static_cast<StorageBucketCallOp>(op);
  bool valid = false;
  if (call.op == StorageBucketCallOp::kRelease) {
    valid = reader.U64(call.bucket);
  } else {
    valid = reader.U64(call.question) &&
            (!NamesBucket(call.op) || reader.U64(call.bucket));
  }
  switch (call.op) {
    case StorageBucketCallOp::kOpen: {
      uint8_t flags = 0;
      valid = valid && reader.Sized(call.name) && reader.Byte(flags) &&
              (flags & ~kAllFlags) == 0 &&
              ((flags & kHasPersisted) || !(flags & kPersisted)) &&
              ((flags & kHasDurability) || !(flags & kStrict));
      if (valid && (flags & kHasPersisted)) {
        call.persisted = (flags & kPersisted) != 0;
      }
      if (valid && (flags & kHasDurability)) {
        call.durability = (flags & kStrict) ? 1 : 0;
      }
      if (valid && (flags & kHasQuota)) {
        uint64_t quota = 0;
        valid = reader.U64(quota);
        call.quota = quota;
      }
      if (valid && (flags & kHasExpires)) {
        int64_t expires = 0;
        valid = reader.I64(expires);
        call.expires = expires;
      }
      break;
    }
    case StorageBucketCallOp::kDelete:
      valid = valid && reader.Sized(call.name);
      break;
    case StorageBucketCallOp::kSetExpires: {
      int64_t expires = 0;
      valid = valid && reader.I64(expires);
      call.expires = expires;
      break;
    }
    default:
      break;
  }
  if (!valid || !reader.AtEnd()) {
    return base::unexpected(
        "a Storage Buckets call that is not what its operation says");
  }
  return call;
}

std::string EncodeStorageBucketAnswer(const PageStorageBucketAnswer& answer) {
  MessageWriter writer;
  writer.Byte(static_cast<uint8_t>(answer.op));
  writer.U64(answer.question);
  if (answer.op == StorageBucketAnswerOp::kOpened) {
    writer.Byte(static_cast<uint8_t>(answer.error));
    writer.U64(answer.bucket);
    return writer.Take();
  }
  writer.Bool(answer.success);
  switch (answer.op) {
    case StorageBucketAnswerOp::kNames:
      writer.U32(static_cast<uint32_t>(answer.names.size()));
      for (const std::string& name : answer.names) {
        writer.Sized(name);
      }
      break;
    case StorageBucketAnswerOp::kBoolean:
      writer.Bool(answer.value);
      break;
    case StorageBucketAnswerOp::kEstimated:
      writer.I64(answer.usage);
      writer.I64(answer.quota);
      break;
    case StorageBucketAnswerOp::kDurability:
      writer.Byte(answer.durability);
      break;
    case StorageBucketAnswerOp::kExpires:
      writer.Bool(answer.expires.has_value());
      if (answer.expires) {
        writer.I64(*answer.expires);
      }
      break;
    default:
      break;
  }
  return writer.Take();
}

bool StorageBucketAnswerFits(const PageStorageBucketAnswer& answer) {
  return EncodeStorageBucketAnswer(answer).size() <= kMaxPayloadSize;
}

base::expected<PageStorageBucketAnswer, std::string> ReadStorageBucketAnswer(
    std::string_view payload) {
  MessageReader reader(payload);
  PageStorageBucketAnswer answer;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(StorageBucketAnswerOp::kMaxValue)) {
    return base::unexpected("a Storage Buckets answer of an unknown operation");
  }
  answer.op = static_cast<StorageBucketAnswerOp>(op);
  bool valid = reader.U64(answer.question);
  if (answer.op == StorageBucketAnswerOp::kOpened) {
    uint8_t error = 0;
    valid = valid && reader.Byte(error) &&
            error <= static_cast<uint8_t>(StorageBucketError::kMaxValue) &&
            reader.U64(answer.bucket) &&
            // A bucket comes exactly when no error does.
            (error == 0) == (answer.bucket != 0);
    answer.error = static_cast<StorageBucketError>(error);
  } else {
    valid = valid && reader.Bool(answer.success);
    switch (answer.op) {
      case StorageBucketAnswerOp::kNames: {
        uint32_t count = 0;
        valid = valid && reader.Count(count, 4);
        for (uint32_t i = 0; valid && i < count; ++i) {
          valid = reader.Sized(answer.names.emplace_back());
        }
        break;
      }
      case StorageBucketAnswerOp::kBoolean:
        valid = valid && reader.Bool(answer.value);
        break;
      case StorageBucketAnswerOp::kEstimated:
        valid = valid && reader.I64(answer.usage) && reader.I64(answer.quota);
        break;
      case StorageBucketAnswerOp::kDurability:
        valid =
            valid && reader.Byte(answer.durability) && answer.durability <= 1;
        break;
      case StorageBucketAnswerOp::kExpires: {
        bool has_expires = false;
        valid = valid && reader.Bool(has_expires);
        if (valid && has_expires) {
          int64_t expires = 0;
          valid = reader.I64(expires);
          answer.expires = expires;
        }
        break;
      }
      default:
        break;
    }
  }
  if (!valid || !reader.AtEnd()) {
    return base::unexpected(
        "a Storage Buckets answer that is not what its operation says");
  }
  return answer;
}

}  // namespace mitto
