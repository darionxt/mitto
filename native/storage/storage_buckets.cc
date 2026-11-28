#include "mitto/storage/storage_buckets.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/storage/storage_key.h"

namespace mitto {

namespace {

// Between a serialized storage key and a bucket's ID. A serialized key holds
// no "^" but before Blink's encoded attributes, which are digits.
constexpr std::string_view kBucketIdSeparator = "^b";

}  // namespace

std::string BucketKey(const BucketInfo& bucket) {
  if (bucket.is_default()) {
    return bucket.storage_key;
  }
  return base::StrCat({bucket.storage_key, kBucketIdSeparator,
                       base::NumberToString(bucket.id)});
}

std::pair<std::string_view, std::optional<int64_t>> ParseBucketKey(
    std::string_view bucket_key) {
  const size_t separator = bucket_key.rfind(kBucketIdSeparator);
  int64_t id = 0;
  if (separator == std::string_view::npos ||
      !base::StringToInt64(
          bucket_key.substr(separator + kBucketIdSeparator.size()), &id)) {
    return {bucket_key, std::nullopt};
  }
  return {bucket_key.substr(0, separator), id};
}

StorageBuckets::StorageBuckets(
    std::optional<base::FilePath> directory,
    scoped_refptr<base::SequencedTaskRunner> database_runner)
    : database_(std::move(database_runner)), directory_(std::move(directory)) {}

StorageBuckets::~StorageBuckets() = default;

void StorageBuckets::Open(base::OnceCallback<void(bool)> opened) {
  database_.AsyncCall(&BucketDatabase::Open)
      .WithArgs(directory_ ? directory_->Append(kBucketDatabaseFileName)
                           : base::FilePath())
      .Then(base::BindOnce(
          [](base::WeakPtr<StorageBuckets> self,
             base::OnceCallback<void(bool)> opened, bool result) {
            if (!self) {
              return;
            }
            if (!result) {
              std::move(opened).Run(false);
              return;
            }
            self->database_.AsyncCall(&BucketDatabase::ReadAll)
                .Then(base::BindOnce(&StorageBuckets::DidRead, self,
                                     std::move(opened)));
          },
          weak_factory_.GetWeakPtr(), std::move(opened)));
}

void StorageBuckets::DidRead(base::OnceCallback<void(bool)> opened,
                             std::optional<BucketDatabase::Contents> contents) {
  if (!contents) {
    std::move(opened).Run(false);
    return;
  }
  last_id_ = contents->last_id;
  for (BucketInfo& bucket : contents->buckets) {
    last_id_ = std::max(last_id_, bucket.id);
    by_name_.emplace(std::make_pair(bucket.storage_key, bucket.name),
                     bucket.id);
    const int64_t id = bucket.id;
    buckets_.emplace(id, std::move(bucket));
  }
  std::move(opened).Run(true);
}

const BucketInfo* StorageBuckets::Find(const std::string& storage_key,
                                       const std::string& name) const {
  const auto found = by_name_.find(std::make_pair(storage_key, name));
  return found == by_name_.end() ? nullptr : FindById(found->second);
}

const BucketInfo* StorageBuckets::FindById(int64_t id) const {
  const auto found = buckets_.find(id);
  return found == buckets_.end() ? nullptr : &found->second;
}

std::vector<const BucketInfo*> StorageBuckets::BucketsOf(
    const std::string& storage_key) const {
  std::vector<const BucketInfo*> buckets;
  for (auto it = by_name_.lower_bound(std::make_pair(storage_key, ""));
       it != by_name_.end() && it->first.first == storage_key; ++it) {
    buckets.push_back(FindById(it->second));
  }
  return buckets;
}

void StorageBuckets::ForEach(
    base::FunctionRef<void(const BucketInfo&)> visit) const {
  for (const auto& [id, bucket] : buckets_) {
    visit(bucket);
  }
}

const BucketInfo& StorageBuckets::Add(BucketInfo bucket) {
  CHECK(!Find(bucket.storage_key, bucket.name));
  bucket.id = ++last_id_;
  if (Kept(bucket.storage_key)) {
    // A write that fails leaves the bucket for this Session alone, as one
    // that Chrome's database lost.
    database_.AsyncCall(base::IgnoreResult(&BucketDatabase::Insert))
        .WithArgs(bucket);
  }
  by_name_.emplace(std::make_pair(bucket.storage_key, bucket.name), bucket.id);
  const int64_t id = bucket.id;
  return buckets_.emplace(id, std::move(bucket)).first->second;
}

void StorageBuckets::SetExpiration(int64_t id, base::Time expiration) {
  const auto found = buckets_.find(id);
  CHECK(found != buckets_.end());
  found->second.expiration = expiration;
  if (Kept(found->second.storage_key)) {
    database_.AsyncCall(base::IgnoreResult(&BucketDatabase::UpdateExpiration))
        .WithArgs(id, expiration);
  }
}

void StorageBuckets::Remove(int64_t id) {
  const auto found = buckets_.find(id);
  CHECK(found != buckets_.end());
  if (Kept(found->second.storage_key)) {
    database_.AsyncCall(base::IgnoreResult(&BucketDatabase::Delete))
        .WithArgs(id);
  }
  by_name_.erase(std::make_pair(found->second.storage_key, found->second.name));
  buckets_.erase(found);
}

bool StorageBuckets::Kept(const std::string& storage_key) const {
  return directory_.has_value() && !IsSerializedForOneTopDocument(storage_key);
}

}  // namespace mitto
