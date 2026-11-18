#include "mitto/page_storage_buckets.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "mitto/page_messages.h"
#include "mitto/storage/storage_buckets.h"
#include "mitto/storage_quota.h"

namespace mitto {
namespace {

// The largest quota that Blink sends: StorageBucketOptions' quota is an
// [EnforceRange] unsigned long long.
constexpr uint64_t kMaxSafeInteger = (uint64_t{1} << 53) - 1;

base::Time TimeFromWire(int64_t microseconds) {
  return base::Time::FromDeltaSinceWindowsEpoch(
      base::Microseconds(microseconds));
}

int64_t TimeToWire(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

StorageBucketError ToWire(StorageQuota::BucketError error) {
  switch (error) {
    case StorageQuota::BucketError::kUnknown:
      return StorageBucketError::kUnknown;
    case StorageQuota::BucketError::kQuotaExceeded:
      return StorageBucketError::kQuotaExceeded;
    case StorageQuota::BucketError::kInvalidExpiration:
      return StorageBucketError::kInvalidExpiration;
  }
}

}  // namespace

bool IsValidBucketName(std::string_view name) {
  if (name.empty() || name.size() >= 64) {
    return false;
  }
  for (char c : name) {
    if (!base::IsAsciiLower(c) && !base::IsAsciiDigit(c) && c != '_' &&
        c != '-') {
      return false;
    }
  }
  return name.front() != '_' && name.front() != '-';
}

PageStorageBuckets::Host::Host() = default;
PageStorageBuckets::Host::Host(Host&&) = default;
PageStorageBuckets::Host& PageStorageBuckets::Host::operator=(Host&&) = default;
PageStorageBuckets::Host::~Host() = default;

PageStorageBuckets::PageStorageBuckets(StorageQuota& quota,
                                       Send send,
                                       Refuse refuse)
    : quota_(quota), send_(std::move(send)), refuse_(std::move(refuse)) {}

PageStorageBuckets::~PageStorageBuckets() = default;

void PageStorageBuckets::OnCall(PageStorageBucketCall call,
                                uint64_t context,
                                const std::optional<std::string>& storage_key) {
  if (call.op == StorageBucketCallOp::kRelease) {
    const auto found = handles_.find(call.bucket);
    if (found == handles_.end() || found->second.context != context) {
      refuse_.Run(base::StrCat({"a release of bucket ",
                                base::NumberToString(call.bucket),
                                ", which the Page was not given"}));
      return;
    }
    ReleaseHandle(call.bucket);
    return;
  }
  if (call.question <= last_question_) {
    refuse_.Run(base::StrCat({"a Storage Buckets question numbered ",
                              base::NumberToString(call.question),
                              ", not larger than the one before"}));
    return;
  }
  last_question_ = call.question;
  if (!storage_key) {
    // Blink throws a SecurityError before it asks, for a context whose origin
    // is opaque, and the interfaces are not there in a context that is not
    // secure, which BucketManager takes for a bad message.
    refuse_.Run(
        "a Storage Buckets call from a context that may not reach buckets");
    return;
  }
  switch (call.op) {
    case StorageBucketCallOp::kOpen:
      OnOpen(std::move(call), context, *storage_key);
      return;
    case StorageBucketCallOp::kKeys:
      quota_->GetBucketNames(
          *storage_key,
          base::BindOnce(
              [](base::WeakPtr<PageStorageBuckets> self, uint64_t question,
                 std::vector<std::string> names) {
                if (!self) {
                  return;
                }
                PageStorageBucketAnswer answer;
                answer.op = StorageBucketAnswerOp::kNames;
                answer.question = question;
                answer.success = true;
                answer.names = std::move(names);
                // More names than a frame holds, which Chrome's pipe would
                // carry, fail as the browser's error does.
                if (!StorageBucketAnswerFits(answer)) {
                  answer.success = false;
                  answer.names.clear();
                }
                self->SendAnswer(answer);
              },
              weak_factory_.GetWeakPtr(), call.question));
      return;
    case StorageBucketCallOp::kDelete:
      if (!IsValidBucketName(call.name)) {
        refuse_.Run("a delete of a bucket whose name is not valid");
        return;
      }
      quota_->DeleteBucket(
          *storage_key, call.name,
          base::BindOnce(
              [](base::WeakPtr<PageStorageBuckets> self, uint64_t question) {
                if (!self) {
                  return;
                }
                PageStorageBucketAnswer answer;
                answer.op = StorageBucketAnswerOp::kDone;
                answer.question = question;
                answer.success = true;
                self->SendAnswer(answer);
              },
              weak_factory_.GetWeakPtr(), call.question));
      return;
    default:
      break;
  }
  const auto found = handles_.find(call.bucket);
  if (found == handles_.end() || found->second.context != context) {
    refuse_.Run(base::StrCat({"a Storage Buckets call of bucket ",
                              base::NumberToString(call.bucket),
                              ", which the Page was not given"}));
    return;
  }
  const int64_t id = found->second.id;
  OnBucketCall(call, hosts_.at(id), id);
}

void PageStorageBuckets::OnOpen(PageStorageBucketCall call,
                                uint64_t context,
                                const std::string& storage_key) {
  if (!IsValidBucketName(call.name)) {
    refuse_.Run("an open of a bucket whose name is not valid");
    return;
  }
  if (call.quota && (*call.quota == 0 || *call.quota > kMaxSafeInteger)) {
    refuse_.Run("an open of a bucket with a quota that Blink does not send");
    return;
  }
  StorageQuota::BucketInitParams params;
  params.storage_key = storage_key;
  params.name = call.name;
  if (call.expires) {
    params.expiration = TimeFromWire(*call.expires);
  }
  params.quota = static_cast<int64_t>(call.quota.value_or(0));
  if (call.durability) {
    params.durability = *call.durability == 1 ? BucketDurability::kStrict
                                              : BucketDurability::kRelaxed;
  }
  // `persisted` takes effect only with the persistent storage permission,
  // which a Page never has.
  ++pending_opens_[context];
  quota_->UpdateOrCreateBucket(
      std::move(params),
      base::BindOnce(
          [](base::WeakPtr<PageStorageBuckets> self, uint64_t question,
             uint64_t context,
             base::expected<BucketInfo, StorageQuota::BucketError> result) {
            if (!self) {
              return;
            }
            PageStorageBucketAnswer answer;
            answer.op = StorageBucketAnswerOp::kOpened;
            answer.question = question;
            // BucketManagerHost::DidGetBucket() of a context that has gone,
            // whose remote would go with it.
            const auto pending = self->pending_opens_.find(context);
            const bool gone = self->gone_with_opens_.contains(context);
            if (--pending->second == 0) {
              self->pending_opens_.erase(pending);
              self->gone_with_opens_.erase(context);
            }
            if (gone) {
              answer.error = StorageBucketError::kUnknown;
              self->SendAnswer(answer);
              return;
            }
            if (!result.has_value()) {
              answer.error = ToWire(result.error());
              self->SendAnswer(answer);
              return;
            }
            // A bucket that the Page already reaches keeps its host, with
            // what the host last read of it.
            auto [host, added] = self->hosts_.try_emplace(result->id);
            if (added) {
              host->second.info = *result;
            }
            ++host->second.handles;
            answer.bucket = self->next_bucket_++;
            self->handles_.emplace(
                answer.bucket, Handle{context, result->id, BucketKey(*result)});
            self->SendAnswer(answer);
          },
          weak_factory_.GetWeakPtr(), call.question, context));
}

void PageStorageBuckets::OnBucketCall(const PageStorageBucketCall& call,
                                      Host& host,
                                      int64_t id) {
  PageStorageBucketAnswer answer;
  answer.question = call.question;
  switch (call.op) {
    case StorageBucketCallOp::kPersist:
      answer.op = StorageBucketAnswerOp::kBoolean;
      // BucketHost::Persist(): from what it last read, since only the
      // permission, which a Page never has, would write.
      answer.success = host.info.has_value();
      answer.value = host.info && host.info->persistent;
      SendAnswer(answer);
      return;
    case StorageBucketCallOp::kPersisted:
      answer.op = StorageBucketAnswerOp::kBoolean;
      answer.success = host.info && Refresh(id);
      answer.value = answer.success && hosts_.at(id).info->persistent;
      SendAnswer(answer);
      return;
    case StorageBucketCallOp::kDurability:
      answer.op = StorageBucketAnswerOp::kDurability;
      answer.success = host.info && Refresh(id);
      answer.durability =
          answer.success ? static_cast<uint8_t>(hosts_.at(id).info->durability)
                         : 0;
      SendAnswer(answer);
      return;
    case StorageBucketCallOp::kExpires:
      answer.op = StorageBucketAnswerOp::kExpires;
      answer.success = host.info && Refresh(id);
      if (answer.success && !hosts_.at(id).info->expiration.is_null()) {
        answer.expires = TimeToWire(hosts_.at(id).info->expiration);
      }
      SendAnswer(answer);
      return;
    case StorageBucketCallOp::kSetExpires: {
      // Even of a host that has forgotten its bucket: it writes by the ID.
      answer.op = StorageBucketAnswerOp::kDone;
      host.info =
          quota_->UpdateBucketExpiration(id, TimeFromWire(*call.expires));
      answer.success = host.info.has_value();
      SendAnswer(answer);
      return;
    }
    case StorageBucketCallOp::kEstimate:
      if (!host.info) {
        answer.op = StorageBucketAnswerOp::kEstimated;
        SendAnswer(answer);
        return;
      }
      quota_->GetBucketUsageAndReportedQuota(
          id,
          base::BindOnce(
              [](base::WeakPtr<PageStorageBuckets> self, uint64_t question,
                 int64_t id,
                 std::optional<StorageQuota::UsageAndQuota> usage_and_quota) {
                if (!self) {
                  return;
                }
                PageStorageBucketAnswer answer;
                answer.op = StorageBucketAnswerOp::kEstimated;
                answer.question = question;
                answer.success = usage_and_quota.has_value();
                if (usage_and_quota) {
                  answer.usage = usage_and_quota->usage;
                  answer.quota = usage_and_quota->reported_quota;
                } else if (const auto host = self->hosts_.find(id);
                           host != self->hosts_.end()) {
                  host->second.info.reset();
                }
                self->SendAnswer(answer);
              },
              weak_factory_.GetWeakPtr(), call.question, id));
      return;
    default:
      NOTREACHED();
  }
}

bool PageStorageBuckets::Refresh(int64_t id) {
  Host& host = hosts_.at(id);
  host.info = quota_->GetBucketById(id);
  return host.info.has_value();
}

std::optional<PageStorageBuckets::Target> PageStorageBuckets::Locate(
    uint64_t bucket,
    uint64_t context) {
  const auto found = handles_.find(bucket);
  if (found == handles_.end() || found->second.context != context) {
    refuse_.Run(base::StrCat({"a call of the storage of bucket ",
                              base::NumberToString(bucket),
                              ", which the Page was not given"}));
    return std::nullopt;
  }
  return Target{found->second.bucket_key,
                quota_->IsBucketKeyLive(found->second.bucket_key)};
}

void PageStorageBuckets::ForgetContexts(const std::set<uint64_t>& contexts) {
  for (uint64_t context : contexts) {
    if (pending_opens_.contains(context)) {
      gone_with_opens_.insert(context);
    }
  }
  std::vector<uint64_t> gone;
  for (const auto& [bucket, handle] : handles_) {
    if (contexts.contains(handle.context)) {
      gone.push_back(bucket);
    }
  }
  for (uint64_t bucket : gone) {
    ReleaseHandle(bucket);
  }
}

void PageStorageBuckets::ReleaseHandle(uint64_t bucket) {
  const auto found = handles_.find(bucket);
  const auto host = hosts_.find(found->second.id);
  if (--host->second.handles == 0) {
    // BucketHost goes with its last receiver.
    hosts_.erase(host);
  }
  handles_.erase(found);
}

void PageStorageBuckets::SendAnswer(const PageStorageBucketAnswer& answer) {
  send_.Run(EncodeStorageBucketsAnswer(answer));
}

}  // namespace mitto
