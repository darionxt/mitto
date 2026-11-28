// The buckets of a Session's storage keys, on the network thread: what
// BucketDatabase keeps, held in memory so that each question is answered at
// once, and each change written through to the database, in order. A storage
// key's default bucket keeps its IndexedDB and Cache API under the serialized
// storage key, as before buckets, and every other bucket under the key and
// its ID, which the Session never gives another bucket.
//
// The buckets of a storage key under a top-level document of an opaque
// origin, or of a credentialless frame, stay in memory, as its storage does.

#ifndef MITTO_STORAGE_STORAGE_BUCKETS_H_
#define MITTO_STORAGE_STORAGE_BUCKETS_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "mitto/storage/bucket_database.h"

namespace mitto {

// What keys `bucket`'s IndexedDB and Cache API in the Session.
std::string BucketKey(const BucketInfo& bucket);

// The serialized storage key of a key that BucketKey() gave, and the
// bucket's ID, or nullopt for a storage key's default bucket.
std::pair<std::string_view, std::optional<int64_t>> ParseBucketKey(
    std::string_view bucket_key);

class StorageBuckets {
 public:
  // `directory` is the Session's storage directory, or nullopt to keep the
  // buckets in memory. `database_runner` may block.
  StorageBuckets(std::optional<base::FilePath> directory,
                 scoped_refptr<base::SequencedTaskRunner> database_runner);
  StorageBuckets(const StorageBuckets&) = delete;
  StorageBuckets& operator=(const StorageBuckets&) = delete;
  ~StorageBuckets();

  // Opens the database and reads every bucket, and says whether it could.
  // Called once, before anything else.
  void Open(base::OnceCallback<void(bool)> opened);

  const BucketInfo* Find(const std::string& storage_key,
                         const std::string& name) const;
  const BucketInfo* FindById(int64_t id) const;
  // The buckets of `storage_key`, by their names in the order of their bytes.
  std::vector<const BucketInfo*> BucketsOf(
      const std::string& storage_key) const;
  void ForEach(base::FunctionRef<void(const BucketInfo&)> visit) const;

  // Adds `bucket` with an ID of its own, and returns it.
  const BucketInfo& Add(BucketInfo bucket);
  void SetExpiration(int64_t id, base::Time expiration);
  void Remove(int64_t id);

 private:
  // Whether the buckets of `storage_key` go into the database.
  bool Kept(const std::string& storage_key) const;
  void DidRead(base::OnceCallback<void(bool)> opened,
               std::optional<BucketDatabase::Contents> contents);

  base::SequenceBound<BucketDatabase> database_;
  const std::optional<base::FilePath> directory_;
  std::map<int64_t, BucketInfo> buckets_;
  std::map<std::pair<std::string, std::string>, int64_t> by_name_;
  int64_t last_id_ = 0;
  base::WeakPtrFactory<StorageBuckets> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_STORAGE_STORAGE_BUCKETS_H_
