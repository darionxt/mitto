// A Session's IndexedDB: the databases of each bucket of each storage key,
// which every Page of the Session shares, as Chrome's IndexedDBContextImpl
// keeps a profile's. Each bucket goes by the key that BucketKey() gives it,
// which is the serialized storage key for a key's default bucket. A Session
// with a storage directory keeps them in its file IndexedDB there, and loads
// each storage key's once, before its first open, delete or list, into memory,
// which every commit writes through to the file from, in order. Without a
// directory, and for a frame under a top-level document of an opaque origin
// or a credentialless frame, they are kept in memory until the Session goes,
// with the locks, value rules and quota of a profile on disk, as a normal
// window has them.
//
// Upstream: content/browser/indexed_db/indexed_db_context_impl.cc

#ifndef MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_
#define MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "mitto/indexed_db/indexed_db_file.h"
#include "mitto/indexed_db/kv_store.h"
#include "mitto/storage_quota.h"

namespace mitto {

class IndexedDbBucket;

class IndexedDbContext final : public StorageQuota::Client {
 public:
  // `directory` is the Session's storage directory, or nullopt to keep the
  // databases in memory. `file_runner` is a sequence that may block, which
  // reads and writes the file.
  IndexedDbContext(StorageQuota& quota,
                   std::optional<base::FilePath> directory,
                   scoped_refptr<base::SequencedTaskRunner> file_runner);
  IndexedDbContext(const IndexedDbContext&) = delete;
  IndexedDbContext& operator=(const IndexedDbContext&) = delete;
  ~IndexedDbContext() override;

  // Opens the file of a Session with a storage directory, and says whether it
  // could. Called once, before anything else.
  void Open(base::OnceCallback<void(bool)> opened);

  // Calls `use` with the databases of `storage_key`, as StorageKey::Serialize()
  // writes it, once they have loaded: at once if they have, and otherwise in
  // the order asked.
  void WithBucket(const std::string& storage_key,
                  base::OnceCallback<void(IndexedDbBucket&)> use);

  // Closes every connection, as the end of the Session does, and calls `done`
  // once all that was committed is in the file and the file is closed.
  // Nothing may use the databases after.
  void Close(base::OnceClosure done);

  // StorageQuota::Client:
  StorageQuota::ClientType type() const override;
  void GetUsage(const std::string& storage_key,
                base::OnceCallback<void(int64_t)> done) override;
  // Closes the bucket's connections and cancels its requests, as Chrome's
  // ForceClose() of a bucket that goes does, and deletes its databases.
  void DeleteBucketData(const std::string& bucket_key,
                        base::OnceClosure done) override;

 private:
  class KeyQuota;

  // Whether the databases of `storage_key` go into the file.
  bool KeptInFile(const std::string& storage_key) const;
  IndexedDbBucket& AddBucket(const std::string& storage_key,
                             KvEntries entries,
                             bool write_through);
  // What the file held of `storage_key`, or nullopt if it could not be read.
  void Loaded(const std::string& storage_key, std::optional<KvEntries> entries);
  void Write(const std::string& storage_key, const KvBatch& batch);

  const raw_ref<StorageQuota> quota_;
  const std::optional<base::FilePath> directory_;
  const scoped_refptr<base::SequencedTaskRunner> file_runner_;
  base::SequenceBound<IndexedDbFile> file_;
  std::map<std::string, std::unique_ptr<IndexedDbBucket>> buckets_;
  // What waits for each storage key whose databases are loading.
  std::map<std::string, std::vector<base::OnceCallback<void(IndexedDbBucket&)>>>
      loading_;
  base::WeakPtrFactory<IndexedDbContext> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_INDEXED_DB_CONTEXT_H_
