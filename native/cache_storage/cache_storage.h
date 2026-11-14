// The caches of one storage key, which `caches` gives each document and
// worker of that key: their names in the order they were made, each cache
// with its own backend, and what a page asks of them all, as Chrome's
// browser keeps them. In a directory, an index file lists the caches and the
// directory each keeps its entries in, and a cache's directory goes once the
// cache is deleted and nothing uses it any more.
//
// Upstream: content/browser/cache_storage/cache_storage.cc
// Upstream: content/browser/cache_storage/cache_storage_index.cc

#ifndef MITTO_CACHE_STORAGE_CACHE_STORAGE_H_
#define MITTO_CACHE_STORAGE_CACHE_STORAGE_H_

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
#include "mitto/cache_storage/cache_storage_cache.h"
#include "mitto/cache_storage/cache_storage_scheduler.h"
#include "mitto/cache_storage/cache_storage_types.h"

namespace mitto {

class CacheStorage final : public CacheStorageCache::Observer {
 public:
  using CacheCallback =
      base::OnceCallback<void(CacheStorageCacheHandle, CacheStorageError)>;
  using BoolCallback = base::OnceCallback<void(bool, CacheStorageError)>;
  using ErrorCallback = base::OnceCallback<void(CacheStorageError)>;
  using NamesCallback = base::OnceCallback<void(std::vector<std::u16string>)>;
  using SizeCallback = base::OnceCallback<void(int64_t)>;

  // `directory` is the storage key's own, which it makes when it first needs
  // it, or empty to keep everything in memory. `file_runner` does the
  // blocking work on its files.
  CacheStorage(base::FilePath directory,
               CacheStorageQuota& quota,
               scoped_refptr<base::SequencedTaskRunner> file_runner);
  CacheStorage(const CacheStorage&) = delete;
  CacheStorage& operator=(const CacheStorage&) = delete;
  ~CacheStorage() override;

  void OpenCache(std::u16string cache_name, CacheCallback callback);
  void HasCache(std::u16string cache_name, BoolCallback callback);
  void DoomCache(std::u16string cache_name, ErrorCallback callback);
  void EnumerateCaches(NamesCallback callback);
  // In the cache of that name, or in each in turn until one matches.
  void MatchCache(std::u16string cache_name,
                  CacheRequest request,
                  std::optional<CacheQueryOptions> options,
                  CacheStorageCache::MatchCallback callback);
  void MatchAllCaches(CacheRequest request,
                      std::optional<CacheQueryOptions> options,
                      CacheStorageCache::MatchCallback callback);
  // What the caches hold, with their padding, for the quota.
  void Size(SizeCallback callback);
  // Closes every cache, and calls `callback` once each has written all it
  // holds.
  void Close(base::OnceClosure callback);

  // CacheStorageCache::Observer:
  void CacheSizeUpdated(const CacheStorageCache& cache) override;
  void CacheUnreferenced(CacheStorageCache& cache) override;

 private:
  // CacheStorageIndex::CacheMetadata.
  struct IndexEntry {
    std::u16string name;
    // The name of the cache's directory, a random UUID.
    std::string directory;
    int64_t padding = CacheStorageCache::kSizeUnknown;
  };
  struct LoadedIndex;

  void LazyInit();
  void LazyInitDidLoadIndex(LoadedIndex index);
  // Runs `operation` once the index has loaded.
  void Schedule(CacheStorageScheduler::Mode mode, base::OnceClosure operation,
                int64_t id);

  void OpenCacheImpl(std::u16string cache_name, CacheCallback callback);
  void OpenCacheDidWriteIndex(std::u16string cache_name,
                              CacheCallback callback,
                              bool success);
  void DoomCacheImpl(std::u16string cache_name, ErrorCallback callback);
  void DoomCacheDidWriteIndex(std::u16string cache_name,
                              std::unique_ptr<CacheStorageCache> cache,
                              CacheStorageCacheHandle handle,
                              ErrorCallback callback,
                              bool success);
  void MatchAllCachesImpl(CacheRequest request,
                          std::optional<CacheQueryOptions> options,
                          CacheStorageCache::MatchCallback callback);
  void SizeImpl(SizeCallback callback);
  void CloseImpl(base::OnceClosure callback);

  // The cache of that name, which it opens if it is not open yet.
  CacheStorageCache* GetLoadedCache(const std::u16string& cache_name);
  std::unique_ptr<CacheStorageCache> CreateCache(const IndexEntry& entry);
  // Writes the index, and calls `callback` with whether it did.
  void WriteIndex(base::OnceCallback<void(bool)> callback);
  // A doomed cache that nothing uses goes, with its directory.
  void FinalizeDoomedCache(CacheStorageCache* cache);

  const base::FilePath directory_;
  const raw_ref<CacheStorageQuota> quota_;
  const scoped_refptr<base::SequencedTaskRunner> file_runner_;
  CacheStorageScheduler scheduler_;
  bool initializing_ = false;
  bool initialized_ = false;
  // The caches in the order they were made.
  std::vector<IndexEntry> index_;
  // Those that are open, by name.
  std::map<std::u16string, std::unique_ptr<CacheStorageCache>> caches_;
  // Deleted, but still used through a handle.
  std::map<CacheStorageCache*, std::unique_ptr<CacheStorageCache>> doomed_;
  base::WeakPtrFactory<CacheStorage> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_CACHE_STORAGE_H_
