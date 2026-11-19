// The messages of the Storage Buckets API between a Page and its Session: what
// the Page's StorageBucketManager and StorageBuckets ask, as Blink asks
// Chrome's browser over blink.mojom.BucketManagerHost and BucketHost, and
// what the Session answers. Two types carry them, kStorageBuckets to the
// Session and kStorageBucketsAnswer to the Page, each with an operation first.
// The library reads what a Page sends as hostile input, so each payload is
// checked here before it acts on it.
//
// Where Mojo hands a BucketHost over, the Session names the bucket by a
// number of its own, which the Page's StorageBucket then names in its calls,
// and in the calls of its IndexedDB and Cache API.
//
// docs/runtime.md describes each message.

#ifndef MITTO_STORAGE_BUCKET_MESSAGES_H_
#define MITTO_STORAGE_BUCKET_MESSAGES_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/types/expected.h"

namespace mitto {

// What a Page asks.
enum class StorageBucketCallOp : uint8_t {
  // StorageBucketManager.open(), keys() and delete().
  kOpen = 1,
  kKeys = 2,
  kDelete = 3,
  // A StorageBucket's persist(), persisted(), estimate(), durability(),
  // setExpires() and expires().
  kPersist = 4,
  kPersisted = 5,
  kEstimate = 6,
  kDurability = 7,
  kSetExpires = 8,
  kExpires = 9,
  // The Page no longer uses a bucket that an open gave, as dropping its remote
  // does.
  kRelease = 10,
  kMaxValue = kRelease,
};

// What the Session answers.
enum class StorageBucketAnswerOp : uint8_t {
  // To kOpen: an error, and the bucket's number.
  kOpened = 1,
  // To kKeys: whether it could, and the names.
  kNames = 2,
  // To kDelete and kSetExpires: whether it could.
  kDone = 3,
  // To kPersist and kPersisted: whether it could, and a byte.
  kBoolean = 4,
  // To kEstimate: whether it could, the usage and the quota.
  kEstimated = 5,
  // To kDurability: whether it could, and the durability.
  kDurability = 6,
  // To kExpires: whether it could, and the expiration, if any.
  kExpires = 7,
  kMaxValue = kExpires,
};

// blink.mojom.BucketError, and none.
enum class StorageBucketError : uint8_t {
  kNone = 0,
  kUnknown = 1,
  kQuotaExceeded = 2,
  kInvalidExpiration = 3,
  kMaxValue = kInvalidExpiration,
};

struct PageStorageBucketCall {
  PageStorageBucketCall();
  PageStorageBucketCall(const PageStorageBucketCall&);
  PageStorageBucketCall(PageStorageBucketCall&&);
  PageStorageBucketCall& operator=(const PageStorageBucketCall&);
  PageStorageBucketCall& operator=(PageStorageBucketCall&&);
  ~PageStorageBucketCall();

  StorageBucketCallOp op = StorageBucketCallOp::kOpen;
  // The number of the question, which the answer names: larger than that of
  // each question before it. kRelease has none.
  uint64_t question = 0;
  // The calls of a StorageBucket, and kRelease: the bucket's number.
  uint64_t bucket = 0;
  // kOpen's and kDelete's, in UTF-8.
  std::string name;
  // kOpen's blink.mojom.BucketPolicies: each that the options gave. A
  // durability is 0 for relaxed and 1 for strict.
  std::optional<bool> persisted;
  std::optional<uint64_t> quota;
  std::optional<uint8_t> durability;
  // kOpen's expiration, and kSetExpires's: a count of microseconds since the
  // Windows epoch, as base::Time holds it.
  std::optional<int64_t> expires;
};

struct PageStorageBucketAnswer {
  PageStorageBucketAnswer();
  PageStorageBucketAnswer(const PageStorageBucketAnswer&);
  PageStorageBucketAnswer(PageStorageBucketAnswer&&);
  PageStorageBucketAnswer& operator=(const PageStorageBucketAnswer&);
  PageStorageBucketAnswer& operator=(PageStorageBucketAnswer&&);
  ~PageStorageBucketAnswer();

  StorageBucketAnswerOp op = StorageBucketAnswerOp::kOpened;
  uint64_t question = 0;
  // kOpened's.
  StorageBucketError error = StorageBucketError::kNone;
  uint64_t bucket = 0;
  // Each other's.
  bool success = false;
  std::vector<std::string> names;
  bool value = false;
  int64_t usage = 0;
  int64_t quota = 0;
  uint8_t durability = 0;
  // kExpires's, as a call's.
  std::optional<int64_t> expires;
};

std::string EncodeStorageBucketCall(const PageStorageBucketCall& call);
std::string EncodeStorageBucketAnswer(const PageStorageBucketAnswer& answer);
bool StorageBucketAnswerFits(const PageStorageBucketAnswer& answer);

base::expected<PageStorageBucketCall, std::string> ReadStorageBucketCall(
    std::string_view payload);
base::expected<PageStorageBucketAnswer, std::string> ReadStorageBucketAnswer(
    std::string_view payload);

}  // namespace mitto

#endif  // MITTO_STORAGE_BUCKET_MESSAGES_H_
