// One cache of the Cache API: its entries in a disk_cache backend of its own,
// the simple backend in a directory, or the memory backend without one, as
// Chrome's browser keeps a cache. Each entry's key is its request's URL
// without the fragment, stream 0 holds what CacheMetadata says of its request
// and response, and stream 1 its body. Its operations run as Chrome's
// scheduler runs them: matches and keys alongside each other, and each put
// and delete alone, in the order they came.
//
// Upstream: content/browser/cache_storage/cache_storage_cache.cc

#ifndef MITTO_CACHE_STORAGE_CACHE_STORAGE_CACHE_H_
#define MITTO_CACHE_STORAGE_CACHE_STORAGE_CACHE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/cache_storage/cache_storage_scheduler.h"
#include "mitto/cache_storage/cache_storage_types.h"
#include "net/disk_cache/disk_cache.h"

namespace mitto {

class CacheStorageCache;

// What keeps a cache open: a doomed cache goes once its last handle has.
class CacheStorageCacheHandle {
 public:
  CacheStorageCacheHandle();
  explicit CacheStorageCacheHandle(base::WeakPtr<CacheStorageCache> cache);
  CacheStorageCacheHandle(CacheStorageCacheHandle&& other);
  CacheStorageCacheHandle& operator=(CacheStorageCacheHandle&& other);
  ~CacheStorageCacheHandle();

  CacheStorageCacheHandle Clone() const;
  CacheStorageCache* value() const { return cache_.get(); }

 private:
  base::WeakPtr<CacheStorageCache> cache_;
};

// The body of a put, which the Page sends as the cache asks for it.
class CacheBodySource {
 public:
  virtual ~CacheBodySource() = default;

  // Calls `done` with the next bytes of the body, which are never empty, or
  // with nullopt if the rest cannot come, such as when the Page has gone.
  virtual void ReadNext(
      base::OnceCallback<void(std::optional<std::string>)> done) = 0;
};

// What a cache asks of the quota of its storage key.
class CacheStorageQuota {
 public:
  // Calls `done` with how many bytes the storage key may still write, or
  // nullopt if that cannot be known, as QuotaManager's
  // GetBucketSpaceRemaining() does.
  virtual void GetSpaceRemaining(
      base::OnceCallback<void(std::optional<int64_t>)> done) = 0;
  // The storage key's caches hold `delta` more bytes.
  virtual void NotifyModified(int64_t delta) = 0;

 protected:
  virtual ~CacheStorageQuota() = default;
};

class CacheStorageCache {
 public:
  // What a cache tells its storage key's list.
  class Observer {
   public:
    virtual void CacheSizeUpdated(const CacheStorageCache& cache) = 0;
    // The last handle of the cache has gone.
    virtual void CacheUnreferenced(CacheStorageCache& cache) = 0;

   protected:
    virtual ~Observer() = default;
  };

  // An entry that a query found, with its body open for reading while the
  // entry lives, even after the cache dooms it.
  struct MatchResult {
    MatchResult();
    MatchResult(MatchResult&&);
    MatchResult& operator=(MatchResult&&);
    ~MatchResult();

    CacheRequest request;
    CacheResponse response;
    // For a response with a body.
    disk_cache::ScopedEntryPtr entry;
    // Keeps the cache, whose backend the entry needs, open.
    CacheStorageCacheHandle handle;
  };

  // blink.mojom.BatchOperation.
  struct BatchOperation {
    enum class Type {
      kPut,
      kDelete,
    };

    BatchOperation();
    BatchOperation(BatchOperation&&);
    BatchOperation& operator=(BatchOperation&&);
    ~BatchOperation();

    Type type = Type::kPut;
    CacheRequest request;
    // A put's: the response and its body, if it has one.
    CacheResponse response;
    std::unique_ptr<CacheBodySource> body;
    // A delete's.
    std::optional<CacheQueryOptions> match_options;
  };

  using ErrorCallback = base::OnceCallback<void(CacheStorageError)>;
  using VerboseErrorCallback =
      base::OnceCallback<void(CacheStorageError, std::optional<std::string>)>;
  using MatchCallback =
      base::OnceCallback<void(CacheStorageError, std::optional<MatchResult>)>;
  using MatchAllCallback =
      base::OnceCallback<void(CacheStorageError, std::vector<MatchResult>)>;
  using KeysCallback =
      base::OnceCallback<void(CacheStorageError, std::vector<CacheRequest>)>;
  using SizeCallback = base::OnceCallback<void(int64_t)>;

  // `path` is the directory of its simple backend, or empty for the memory
  // backend. `cache_padding` is what the index says, or kSizeUnknown.
  CacheStorageCache(std::u16string cache_name,
                    base::FilePath path,
                    CacheStorageQuota& quota,
                    Observer* observer,
                    int64_t cache_padding);
  CacheStorageCache(const CacheStorageCache&) = delete;
  CacheStorageCache& operator=(const CacheStorageCache&) = delete;
  ~CacheStorageCache();

  static constexpr int64_t kSizeUnknown = -1;

  CacheStorageCacheHandle CreateHandle();
  const std::u16string& cache_name() const { return cache_name_; }
  const base::FilePath& path() const { return path_; }
  int64_t cache_size() const { return cache_size_; }
  int64_t cache_padding() const { return cache_padding_; }
  int64_t PaddedCacheSize() const;
  void SetObserver(Observer* observer) { observer_ = observer; }
  bool IsUnreferenced() const { return handle_ref_count_ == 0; }

  void Match(CacheRequest request,
             std::optional<CacheQueryOptions> options,
             MatchCallback callback);
  void MatchAll(std::optional<CacheRequest> request,
                std::optional<CacheQueryOptions> options,
                MatchAllCallback callback);
  void Keys(std::optional<CacheRequest> request,
            std::optional<CacheQueryOptions> options,
            KeysCallback callback);
  // Checks the batch for duplicates and the quota, then runs each put or the
  // delete. `bad_message` is called for a batch that no renderer sends.
  void Batch(std::vector<BatchOperation> operations,
                      VerboseErrorCallback callback,
                      base::OnceClosure bad_message);
  void Size(SizeCallback callback);
  // Closes the backend once the operations before have run, and calls
  // `callback` once it has written all it holds.
  void Close(base::OnceClosure callback);

 private:
  friend class CacheStorageCacheHandle;

  enum BackendState {
    kBackendUninitialized,
    kBackendOpen,
    kBackendClosed,
  };

  // What a query returns.
  enum QueryType : uint8_t {
    kQueryEntries = 1 << 0,
    kQueryRequests = 1 << 1,
    kQueryResponsesWithBodies = 1 << 2,
    kQueryResponsesNoBodies = 1 << 3,
  };

  struct QueryResult;
  struct QueryContext;
  struct PutContext;
  struct BatchInfo;
  using QueryResults = std::vector<QueryResult>;
  using QueryCallback =
      base::OnceCallback<void(CacheStorageError, std::unique_ptr<QueryResults>)>;

  void AddHandleRef();
  void DropHandleRef();

  void InitBackend();
  void InitDidCreateBackend(base::OnceClosure callback,
                            disk_cache::BackendResult result);
  void InitGotCacheSize(base::OnceClosure callback, int64_t cache_size);
  void InitGotCacheSizeAndPadding(base::OnceClosure callback,
                                  int64_t cache_size,
                                  int64_t cache_padding);
  void CalculatePadding(int64_t cache_size,
                        base::OnceCallback<void(int64_t, int64_t)> callback);

  void Query(std::optional<CacheRequest> request,
             std::optional<CacheQueryOptions> options,
             uint8_t query_types,
             QueryCallback callback);
  void QueryDidOpenFastPath(std::unique_ptr<QueryContext> context,
                            disk_cache::EntryResult result);
  void QueryOpenNextEntry(std::unique_ptr<QueryContext> context);
  void QueryFilterEntry(std::unique_ptr<QueryContext> context,
                        disk_cache::EntryResult result);
  void QueryDidReadMetadata(std::unique_ptr<QueryContext> context,
                            disk_cache::ScopedEntryPtr entry,
                            std::optional<CacheMetadata> metadata);

  void MatchAllImpl(std::optional<CacheRequest> request,
                    std::optional<CacheQueryOptions> options,
                    MatchAllCallback callback);
  void MatchAllDidQuery(MatchAllCallback callback,
                        CacheStorageError error,
                        std::unique_ptr<QueryResults> results);
  void KeysImpl(std::optional<CacheRequest> request,
                std::optional<CacheQueryOptions> options,
                KeysCallback callback);

  void BatchDidGetSpaceRemaining(std::vector<BatchOperation> operations,
                                 VerboseErrorCallback callback,
                                 std::optional<std::string> message,
                                 uint64_t space_required,
                                 std::optional<int64_t> space_remaining);
  void BatchDidOneOperation(BatchInfo& batch, CacheStorageError error);

  void Put(BatchOperation operation, ErrorCallback callback);
  void PutImpl(std::unique_ptr<PutContext> put);
  void PutDidDeleteEntry(std::unique_ptr<PutContext> put,
                         CacheStorageError error);
  void PutDidCreateEntry(std::unique_ptr<PutContext> put,
                         disk_cache::EntryResult result);
  void PutDidWriteHeaders(std::unique_ptr<PutContext> put,
                          int expected_bytes,
                          int rv);
  void PutReadBody(std::unique_ptr<PutContext> put);
  void PutDidReadBody(std::unique_ptr<PutContext> put,
                      std::optional<std::string> bytes);
  void PutDidWriteBody(std::unique_ptr<PutContext> put,
                       int expected_bytes,
                       int rv);
  void PutComplete(std::unique_ptr<PutContext> put, CacheStorageError error);

  void Delete(BatchOperation operation, ErrorCallback callback);
  void DeleteImpl(CacheRequest request,
                  std::optional<CacheQueryOptions> options,
                  ErrorCallback callback);
  void DeleteDidQuery(ErrorCallback callback,
                      CacheStorageError error,
                      std::unique_ptr<QueryResults> results);

  void UpdateCacheSize(base::OnceClosure callback);
  void UpdateCacheSizeGotSize(base::OnceClosure callback,
                              int64_t current_cache_size);

  void CloseImpl(base::OnceClosure callback);
  void BackendCompletedIO();

  const std::u16string cache_name_;
  const size_t cache_name_utf8_size_;
  const base::FilePath path_;
  const raw_ref<CacheStorageQuota> quota_;
  raw_ptr<Observer> observer_;
  std::unique_ptr<disk_cache::Backend> backend_;
  BackendState backend_state_ = kBackendUninitialized;
  CacheStorageScheduler scheduler_;
  bool initializing_ = false;
  int64_t cache_size_ = 0;
  int64_t cache_padding_ = 0;
  int64_t last_reported_size_ = 0;
  size_t handle_ref_count_ = 0;
  int query_recursive_depth_ = 0;
  // What runs once the backend has written all it holds, after Close().
  base::OnceClosure backend_closed_callback_;
  base::WeakPtrFactory<CacheStorageCache> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_CACHE_STORAGE_CACHE_H_
