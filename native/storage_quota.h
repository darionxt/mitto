// The quota of a Session's storage, as Chrome's QuotaManager enforces it and
// reports it: the buckets of each storage key, what IndexedDB and the Cache
// API hold in each bucket, which a write may not take past the bucket's quota
// or its storage key's, and what navigator.storage.estimate() and a
// StorageBucket's estimate() say. DOM storage has a quota of its own and
// counts for none of it, as in Chrome.
//
// The quota enforced is Chrome's: on disk, 75% of a pool of 80% of the disk
// that holds the Session's storage; in memory, Incognito's, a third of a pool
// of a random 15% to 20% of the memory. What estimate() reports is the usage
// and 10 GiB more, on a disk of 10 GiB or more and always in memory, as
// kStaticStorageQuota has it, or a bucket's own quota.
//
// Upstream: storage/browser/quota/quota_settings.cc
// Upstream: storage/browser/quota/quota_manager_impl.cc

#ifndef MITTO_STORAGE_QUOTA_H_
#define MITTO_STORAGE_QUOTA_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "mitto/storage/bucket_database.h"
#include "mitto/storage/storage_buckets.h"

namespace mitto {

class StorageQuota {
 public:
  // storage::QuotaClientType, of those a Page has.
  enum class ClientType {
    kIndexedDatabase,
    kServiceWorkerCache,
  };

  // What holds a part of the usage, of each bucket by the key that BucketKey()
  // gives it.
  class Client {
   public:
    virtual ClientType type() const = 0;
    // Calls `done` with how many bytes the client holds for `bucket_key`.
    virtual void GetUsage(const std::string& bucket_key,
                          base::OnceCallback<void(int64_t)> done) = 0;
    // Closes what uses the bucket, deletes what it holds of it, and calls
    // `done` once it has, as a QuotaClient's DeleteBucketData() does.
    virtual void DeleteBucketData(const std::string& bucket_key,
                                  base::OnceClosure done) = 0;

   protected:
    virtual ~Client() = default;
  };

  // What estimate() gives, and the quota that writes are held to.
  struct UsageAndQuota {
    int64_t usage = 0;
    int64_t indexed_db = 0;
    int64_t caches = 0;
    // What a write may not go past.
    int64_t quota = 0;
    // What estimate() says the quota is.
    int64_t reported_quota = 0;
  };

  // storage::BucketInitParams: what a page asks of a bucket that it opens.
  struct BucketInitParams {
    BucketInitParams();
    BucketInitParams(const BucketInitParams&);
    BucketInitParams& operator=(const BucketInitParams&);
    ~BucketInitParams();

    std::string storage_key;
    std::string name;
    // Null for none.
    base::Time expiration;
    // 0 for none.
    int64_t quota = 0;
    std::optional<BucketDurability> durability;
  };

  // storage::QuotaError, of those that an open gives a page.
  enum class BucketError {
    kUnknown,
    kQuotaExceeded,
    kInvalidExpiration,
  };

  // `directory` is where the Session keeps its storage, whose disk sets the
  // quota and which keeps the buckets, or nullopt for storage in memory,
  // whose quota `physical_memory` sets. `file_runner` reads the size of the
  // disk, and `database_runner` reads and writes the buckets.
  StorageQuota(std::optional<base::FilePath> directory,
               uint64_t physical_memory,
               scoped_refptr<base::SequencedTaskRunner> file_runner,
               scoped_refptr<base::SequencedTaskRunner> database_runner);
  StorageQuota(const StorageQuota&) = delete;
  StorageQuota& operator=(const StorageQuota&) = delete;
  ~StorageQuota();

  // Reads the buckets, and says whether it could. Called once, before any
  // question of buckets.
  void Open(base::OnceCallback<void(bool)> opened);

  // Each client lives as long as this.
  void RegisterClient(Client& client);

  // UpdateOrCreateBucket(): the bucket of that name of the storage key, made
  // if there is none, or made again if it has expired. Of an existing one,
  // only a later expiration changes.
  void UpdateOrCreateBucket(
      BucketInitParams params,
      base::OnceCallback<void(base::expected<BucketInfo, BucketError>)> done);
  // Makes the default bucket of `storage_key`, if it has none, as Chrome does
  // once IndexedDB or the Cache API first reaches it.
  void EnsureDefaultBucket(const std::string& storage_key);
  std::optional<BucketInfo> GetBucketById(int64_t id) const;
  // The names of the buckets of `storage_key` but its default, in the order of
  // their bytes, once those that have expired have gone, as keys() has them.
  void GetBucketNames(const std::string& storage_key,
                      base::OnceCallback<void(std::vector<std::string>)> done);
  // Deletes the bucket of that name with its data, if there is one, and calls
  // `done` once it has.
  void DeleteBucket(const std::string& storage_key,
                    const std::string& name,
                    base::OnceClosure done);
  // The bucket with its new expiration, or nullopt if it has gone.
  std::optional<BucketInfo> UpdateBucketExpiration(int64_t id,
                                                   base::Time expiration);
  // GetBucketUsageAndReportedQuota(): what a StorageBucket's estimate() says,
  // or nullopt if the bucket has gone.
  void GetBucketUsageAndReportedQuota(
      int64_t id,
      base::OnceCallback<void(std::optional<UsageAndQuota>)> done);
  // Whether the bucket that `bucket_key` names is still there.
  bool IsBucketKeyLive(const std::string& bucket_key) const;

  // What the storage key holds in all its buckets, for
  // navigator.storage.estimate().
  void GetUsageAndQuota(const std::string& storage_key,
                        base::OnceCallback<void(UsageAndQuota)> done);
  // How much the bucket may still write: the least of what its storage key's
  // quota leaves and what its own leaves, or nullopt if it has gone.
  void GetSpaceRemaining(const std::string& bucket_key,
                         base::OnceCallback<void(std::optional<int64_t>)> done);
  // A client holds `delta` more bytes for `bucket_key`.
  void NotifyModified(const std::string& bucket_key,
                      ClientType type,
                      int64_t delta);

 private:
  struct Usage {
    int64_t indexed_db = 0;
    int64_t caches = 0;
  };
  struct KeyState;

  // Calls `done` once the quota is known.
  void WhenQuotaKnown(base::OnceClosure done);
  // Calls `done` once the quota and the usage of each of `bucket_keys` are
  // known.
  void WhenKnown(std::vector<std::string> bucket_keys, base::OnceClosure done);
  void WhenBucketKnown(const std::string& bucket_key, base::OnceClosure done);
  void DidGetTotalDiskSpace(std::optional<int64_t> total);
  void DidGetClientUsage(const std::string& bucket_key,
                         ClientType type,
                         int64_t usage);
  // The keys of the buckets of `storage_key`, its default bucket's first.
  std::vector<std::string> BucketKeysOf(const std::string& storage_key) const;
  Usage UsageOf(const std::vector<std::string>& bucket_keys) const;

  void CreateBucket(
      BucketInitParams params,
      base::OnceCallback<void(base::expected<BucketInfo, BucketError>)> done);
  // Removes the bucket and deletes its data in every client.
  void DeleteBucketData(const BucketInfo& bucket, base::OnceClosure done);
  // Deletes the buckets of `storage_key` that have expired, or of every key
  // for nullopt, and calls `done` once they have gone.
  void DeleteExpiredBuckets(const std::optional<std::string>& storage_key,
                            base::OnceClosure done);
  void EvictExpiredBuckets();

  const std::optional<base::FilePath> directory_;
  const scoped_refptr<base::SequencedTaskRunner> file_runner_;
  const scoped_refptr<base::SequencedTaskRunner> database_runner_;
  StorageBuckets buckets_;
  std::vector<raw_ptr<Client>> clients_;
  // The quota of each storage key, once it is known, and what estimate()
  // reports beside the usage.
  std::optional<int64_t> per_key_quota_;
  int64_t reported_quota_extra_ = 0;
  bool asked_disk_ = false;
  std::vector<base::OnceClosure> waiting_for_quota_;
  // By bucket key.
  std::map<std::string, KeyState> usage_;
  // Chrome's eviction rounds delete the buckets that have expired: the first
  // some minutes after the start, then one each half hour.
  base::OneShotTimer first_eviction_;
  base::RepeatingTimer eviction_;
  base::WeakPtrFactory<StorageQuota> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_STORAGE_QUOTA_H_
