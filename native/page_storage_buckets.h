// The library's side of a Page's Storage Buckets API, on the network thread:
// it serves what the Page's StorageBucketManager and StorageBuckets ask, as
// Chrome's BucketManagerHost and BucketHost serve a renderer, from the
// Session's buckets of the storage key of the context that asks, which the
// library works out itself. Where Mojo hands a BucketHost over, the library
// gives the Page a number for the bucket, which its StorageBucket names in
// its calls and in those of its IndexedDB and Cache API.
//
// As Chrome's BucketHost, the library keeps what it last read of each bucket,
// for every StorageBucket of the Page of that bucket, and answers persist()
// from it: a call that finds the bucket gone forgets it.
//
// Upstream: content/browser/buckets/bucket_manager_host.cc
// Upstream: content/browser/buckets/bucket_host.cc

#ifndef MITTO_PAGE_STORAGE_BUCKETS_H_
#define MITTO_PAGE_STORAGE_BUCKETS_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/storage/bucket_database.h"
#include "mitto/storage_bucket_messages.h"

namespace mitto {

class StorageQuota;

// Chrome's IsValidBucketName(): 1 to 63 of lowercase Latin letters, digits,
// "-" and "_", but not "-" or "_" first.
bool IsValidBucketName(std::string_view name);

class PageStorageBuckets {
 public:
  using Send = base::RepeatingCallback<void(std::string frame)>;
  using Refuse = base::RepeatingCallback<void(std::string why)>;

  PageStorageBuckets(StorageQuota& quota, Send send, Refuse refuse);
  PageStorageBuckets(const PageStorageBuckets&) = delete;
  PageStorageBuckets& operator=(const PageStorageBuckets&) = delete;
  ~PageStorageBuckets();

  // A call of the Page's context `context`, whose storage key is
  // `storage_key`, or nullopt if it may not reach the Storage Buckets API: one
  // of an opaque origin, or not in a secure context.
  void OnCall(PageStorageBucketCall call,
              uint64_t context,
              const std::optional<std::string>& storage_key);

  // Where the IndexedDB or Cache API of bucket `bucket` of context `context`
  // goes: the key of its bucket, and whether the bucket is still there. None,
  // after the Page has been refused, for a bucket that the Page was not
  // given, or gave to another context.
  struct Target {
    std::string bucket_key;
    bool live = false;
  };
  std::optional<Target> Locate(uint64_t bucket, uint64_t context);

  // The Page's contexts `contexts` have gone, and their buckets with them, as
  // a renderer's remotes go with its context.
  void ForgetContexts(const std::set<uint64_t>& contexts);

 private:
  // A StorageBucket's BucketHost remote.
  struct Handle {
    uint64_t context = 0;
    int64_t id = 0;
    std::string bucket_key;
  };
  // A BucketHost: what it last read of its bucket, none once a call found the
  // bucket gone, and how many of the Page's StorageBuckets it serves.
  struct Host {
    Host();
    Host(Host&&);
    Host& operator=(Host&&);
    ~Host();

    std::optional<BucketInfo> info;
    size_t handles = 0;
  };

  void OnOpen(PageStorageBucketCall call,
              uint64_t context,
              const std::string& storage_key);
  void OnBucketCall(const PageStorageBucketCall& call, Host& host, int64_t id);
  void ReleaseHandle(uint64_t bucket);
  // Reads bucket `id` again for its host, as BucketHost::DidGetBucket() does,
  // and returns whether it is still there.
  bool Refresh(int64_t id);
  void SendAnswer(const PageStorageBucketAnswer& answer);

  const raw_ref<StorageQuota> quota_;
  const Send send_;
  const Refuse refuse_;
  uint64_t last_question_ = 0;
  // The buckets the Page was given, by the number the library gave each.
  std::map<uint64_t, Handle> handles_;
  uint64_t next_bucket_ = 1;
  // By bucket ID.
  std::map<int64_t, Host> hosts_;
  // How many opens each context waits for, and the contexts among them that
  // have gone, whose opens give no bucket.
  std::map<uint64_t, size_t> pending_opens_;
  std::set<uint64_t> gone_with_opens_;
  base::WeakPtrFactory<PageStorageBuckets> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_STORAGE_BUCKETS_H_
