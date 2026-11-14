#include "mitto/cache_storage/cache_storage.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/barrier_callback.h"
#include "base/barrier_closure.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/pickle.h"
#include "base/task/task_runner.h"
#include "base/uuid.h"

namespace mitto {
namespace {

// The file that lists a storage key's caches.
constexpr base::FilePath::CharType kIndexFileName[] = FILE_PATH_LITERAL("index");

// Changes with any change to what the index holds.
constexpr int kIndexVersion = 1;

std::string SerializeIndex(const std::vector<std::u16string>& names,
                           const std::vector<std::string>& directories,
                           const std::vector<int64_t>& paddings) {
  base::Pickle pickle;
  pickle.WriteInt(kIndexVersion);
  pickle.WriteUInt64(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    pickle.WriteString16(names[i]);
    pickle.WriteString(directories[i]);
    pickle.WriteInt64(paddings[i]);
  }
  return std::string(pickle.AsStringView());
}

}  // namespace

struct CacheStorage::LoadedIndex {
  std::vector<IndexEntry> entries;
};

CacheStorage::CacheStorage(base::FilePath directory,
                           CacheStorageQuota& quota,
                           scoped_refptr<base::SequencedTaskRunner> file_runner)
    : directory_(std::move(directory)),
      quota_(quota),
      file_runner_(std::move(file_runner)) {}

CacheStorage::~CacheStorage() = default;

void CacheStorage::LazyInit() {
  if (initialized_ || initializing_) {
    return;
  }
  initializing_ = true;
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(
          [](base::WeakPtr<CacheStorage> self, int64_t id) {
            if (!self) {
              return;
            }
            if (self->directory_.empty()) {
              self->LazyInitDidLoadIndex(LoadedIndex());
              self->scheduler_.CompleteOperationAndRunNext(id);
              return;
            }
            // SimpleCacheLoader::LoadIndex(), which also deletes the
            // directories that no cache of the index names, such as those of
            // a cache that was deleted while something still used it.
            self->file_runner_->PostTaskAndReplyWithResult(
                FROM_HERE,
                base::BindOnce(
                    [](const base::FilePath& directory) {
                      LoadedIndex index;
                      std::string bytes;
                      if (base::ReadFileToString(
                              directory.Append(kIndexFileName), &bytes)) {
                        base::PickleIterator iterator =
                            base::PickleIterator::WithData(
                                base::as_byte_span(bytes));
                        int version = 0;
                        uint64_t count = 0;
                        if (iterator.ReadInt(&version) &&
                            version == kIndexVersion &&
                            iterator.ReadUInt64(&count)) {
                          for (uint64_t i = 0; i < count; ++i) {
                            IndexEntry entry;
                            if (!iterator.ReadString16(&entry.name) ||
                                !iterator.ReadString(&entry.directory) ||
                                !iterator.ReadInt64(&entry.padding) ||
                                !base::Uuid::ParseLowercase(entry.directory)
                                     .is_valid()) {
                              index.entries.clear();
                              break;
                            }
                            index.entries.push_back(std::move(entry));
                          }
                        }
                      }
                      std::set<std::string> kept;
                      for (const IndexEntry& entry : index.entries) {
                        kept.insert(entry.directory);
                      }
                      base::FileEnumerator directories(
                          directory, /*recursive=*/false,
                          base::FileEnumerator::DIRECTORIES);
                      for (base::FilePath path = directories.Next();
                           !path.empty(); path = directories.Next()) {
                        const std::string name =
                            path.BaseName().AsUTF8Unsafe();
                        if (base::Uuid::ParseLowercase(name).is_valid() &&
                            !kept.contains(name)) {
                          base::DeletePathRecursively(path);
                        }
                      }
                      return index;
                    },
                    self->directory_),
                base::BindOnce(
                    [](base::WeakPtr<CacheStorage> self, int64_t id,
                       LoadedIndex index) {
                      if (!self) {
                        return;
                      }
                      self->LazyInitDidLoadIndex(std::move(index));
                      self->scheduler_.CompleteOperationAndRunNext(id);
                    },
                    self, id));
          },
          weak_factory_.GetWeakPtr(), id));
}

void CacheStorage::LazyInitDidLoadIndex(LoadedIndex index) {
  index_ = std::move(index.entries);
  initializing_ = false;
  initialized_ = true;
}

void CacheStorage::Schedule(CacheStorageScheduler::Mode mode,
                            base::OnceClosure operation,
                            int64_t id) {
  LazyInit();
  scheduler_.ScheduleOperation(id, mode, std::move(operation));
}

void CacheStorage::OpenCache(std::u16string cache_name,
                             CacheCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kExclusive,
           base::BindOnce(&CacheStorage::OpenCacheImpl,
                          weak_factory_.GetWeakPtr(), std::move(cache_name),
                          scheduler_.WrapCallbackToRunNext(id,
                                                           std::move(callback))),
           id);
}

void CacheStorage::OpenCacheImpl(std::u16string cache_name,
                                 CacheCallback callback) {
  if (CacheStorageCache* cache = GetLoadedCache(cache_name)) {
    std::move(callback).Run(cache->CreateHandle(), CacheStorageError::kSuccess);
    return;
  }
  IndexEntry entry;
  entry.name = cache_name;
  entry.directory = base::Uuid::GenerateRandomV4().AsLowercaseString();
  entry.padding = 0;
  std::unique_ptr<CacheStorageCache> cache = CreateCache(entry);
  index_.push_back(std::move(entry));
  caches_.emplace(cache_name, std::move(cache));
  WriteIndex(base::BindOnce(&CacheStorage::OpenCacheDidWriteIndex,
                            weak_factory_.GetWeakPtr(), std::move(cache_name),
                            std::move(callback)));
}

void CacheStorage::OpenCacheDidWriteIndex(std::u16string cache_name,
                                          CacheCallback callback,
                                          bool success) {
  // CreateCacheDidWriteIndex(): the cache is open whether or not the index
  // could be written.
  CacheStorageCache* const cache = GetLoadedCache(cache_name);
  CHECK(cache);
  std::move(callback).Run(cache->CreateHandle(), CacheStorageError::kSuccess);
}

void CacheStorage::HasCache(std::u16string cache_name, BoolCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kShared,
           base::BindOnce(
               [](base::WeakPtr<CacheStorage> self,
                  std::u16string cache_name, BoolCallback callback) {
                 if (!self) {
                   return;
                 }
                 const bool has = std::ranges::any_of(
                     self->index_, [&cache_name](const IndexEntry& entry) {
                       return entry.name == cache_name;
                     });
                 std::move(callback).Run(has, CacheStorageError::kSuccess);
               },
               weak_factory_.GetWeakPtr(), std::move(cache_name),
               scheduler_.WrapCallbackToRunNext(id, std::move(callback))),
           id);
}

void CacheStorage::DoomCache(std::u16string cache_name,
                             ErrorCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kExclusive,
           base::BindOnce(&CacheStorage::DoomCacheImpl,
                          weak_factory_.GetWeakPtr(), std::move(cache_name),
                          scheduler_.WrapCallbackToRunNext(id,
                                                           std::move(callback))),
           id);
}

void CacheStorage::DoomCacheImpl(std::u16string cache_name,
                                 ErrorCallback callback) {
  CacheStorageCache* const cache = GetLoadedCache(cache_name);
  if (!cache) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), CacheStorageError::kErrorNotFound));
    return;
  }
  CacheStorageCacheHandle handle = cache->CreateHandle();
  const auto found = caches_.find(cache_name);
  std::unique_ptr<CacheStorageCache> owned = std::move(found->second);
  caches_.erase(found);
  std::erase_if(index_, [&cache_name](const IndexEntry& entry) {
    return entry.name == cache_name;
  });
  WriteIndex(base::BindOnce(&CacheStorage::DoomCacheDidWriteIndex,
                            weak_factory_.GetWeakPtr(), std::move(cache_name),
                            std::move(owned), std::move(handle),
                            std::move(callback)));
}

void CacheStorage::DoomCacheDidWriteIndex(
    std::u16string cache_name,
    std::unique_ptr<CacheStorageCache> cache,
    CacheStorageCacheHandle handle,
    ErrorCallback callback,
    bool success) {
  if (!success) {
    // RestoreDoomedCache(): it stays, at the end of the list.
    IndexEntry entry;
    entry.name = cache_name;
    entry.directory = cache->path().BaseName().AsUTF8Unsafe();
    entry.padding = cache->cache_padding();
    index_.push_back(std::move(entry));
    caches_.emplace(std::move(cache_name), std::move(cache));
    std::move(callback).Run(CacheStorageError::kErrorStorage);
    return;
  }
  CacheStorageCache* const doomed = cache.get();
  doomed_.emplace(doomed, std::move(cache));
  std::move(callback).Run(CacheStorageError::kSuccess);
  // Dropping the handle finalizes the cache if nothing else uses it.
}

void CacheStorage::EnumerateCaches(NamesCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kShared,
           base::BindOnce(
               [](base::WeakPtr<CacheStorage> self, NamesCallback callback) {
                 if (!self) {
                   return;
                 }
                 std::vector<std::u16string> names;
                 for (const IndexEntry& entry : self->index_) {
                   names.push_back(entry.name);
                 }
                 std::move(callback).Run(std::move(names));
               },
               weak_factory_.GetWeakPtr(),
               scheduler_.WrapCallbackToRunNext(id, std::move(callback))),
           id);
}

void CacheStorage::MatchCache(std::u16string cache_name,
                              CacheRequest request,
                              std::optional<CacheQueryOptions> options,
                              CacheStorageCache::MatchCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(
      CacheStorageScheduler::Mode::kShared,
      base::BindOnce(
          [](base::WeakPtr<CacheStorage> self, std::u16string cache_name,
             CacheRequest request, std::optional<CacheQueryOptions> options,
             CacheStorageCache::MatchCallback callback) {
            if (!self) {
              return;
            }
            CacheStorageCache* const cache = self->GetLoadedCache(cache_name);
            if (!cache) {
              std::move(callback).Run(
                  CacheStorageError::kErrorCacheNameNotFound, std::nullopt);
              return;
            }
            cache->Match(
                std::move(request), std::move(options),
                base::BindOnce(
                    [](CacheStorageCacheHandle,
                       CacheStorageCache::MatchCallback callback,
                       CacheStorageError error,
                       std::optional<CacheStorageCache::MatchResult> match) {
                      std::move(callback).Run(error, std::move(match));
                    },
                    cache->CreateHandle(), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), std::move(cache_name),
          std::move(request), std::move(options),
          scheduler_.WrapCallbackToRunNext(id, std::move(callback))),
      id);
}

void CacheStorage::MatchAllCaches(CacheRequest request,
                                  std::optional<CacheQueryOptions> options,
                                  CacheStorageCache::MatchCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kShared,
           base::BindOnce(&CacheStorage::MatchAllCachesImpl,
                          weak_factory_.GetWeakPtr(), std::move(request),
                          std::move(options),
                          scheduler_.WrapCallbackToRunNext(id,
                                                           std::move(callback))),
           id);
}

void CacheStorage::MatchAllCachesImpl(
    CacheRequest request,
    std::optional<CacheQueryOptions> options,
    CacheStorageCache::MatchCallback callback) {
  using Result =
      std::pair<CacheStorageError, std::optional<CacheStorageCache::MatchResult>>;
  const size_t count = index_.size();
  // Each cache matches at once, and the first in the list that does not
  // answer kErrorNotFound gives the result.
  auto results = std::make_unique<std::vector<Result>>(count);
  std::vector<Result>* const results_ptr = results.get();
  const base::RepeatingClosure barrier = base::BarrierClosure(
      count,
      base::BindOnce(
          [](std::unique_ptr<std::vector<Result>> results,
             CacheStorageCache::MatchCallback callback) {
            for (Result& result : *results) {
              if (result.first == CacheStorageError::kErrorNotFound) {
                continue;
              }
              std::move(callback).Run(result.first, std::move(result.second));
              return;
            }
            std::move(callback).Run(CacheStorageError::kErrorNotFound,
                                    std::nullopt);
          },
          std::move(results), std::move(callback)));
  // The names first, since opening a cache changes nothing in the list.
  std::vector<std::u16string> names;
  for (const IndexEntry& entry : index_) {
    names.push_back(entry.name);
  }
  for (size_t i = 0; i < names.size(); ++i) {
    CacheStorageCache* const cache = GetLoadedCache(names[i]);
    CHECK(cache);
    cache->Match(
        request, options,
        base::BindOnce(
            [](CacheStorageCacheHandle, Result* out,
               base::RepeatingClosure barrier, CacheStorageError error,
               std::optional<CacheStorageCache::MatchResult> match) {
              out->first = error;
              out->second = std::move(match);
              barrier.Run();
            },
            cache->CreateHandle(), &(*results_ptr)[i], barrier));
  }
}

void CacheStorage::Size(SizeCallback callback) {
  const int64_t id = scheduler_.CreateId();
  Schedule(CacheStorageScheduler::Mode::kShared,
           base::BindOnce(&CacheStorage::SizeImpl, weak_factory_.GetWeakPtr(),
                          scheduler_.WrapCallbackToRunNext(id,
                                                           std::move(callback))),
           id);
}

void CacheStorage::SizeImpl(SizeCallback callback) {
  std::vector<std::u16string> names;
  for (const IndexEntry& entry : index_) {
    names.push_back(entry.name);
  }
  const auto barrier = base::BarrierCallback<int64_t>(
      names.size(), base::BindOnce(
                        [](SizeCallback callback, std::vector<int64_t> sizes) {
                          int64_t total = 0;
                          for (int64_t size : sizes) {
                            total += std::max<int64_t>(size, 0);
                          }
                          std::move(callback).Run(total);
                        },
                        std::move(callback)));
  for (const std::u16string& name : names) {
    CacheStorageCache* const cache = GetLoadedCache(name);
    CHECK(cache);
    cache->Size(base::BindOnce(
        [](CacheStorageCacheHandle,
           base::RepeatingCallback<void(int64_t)> barrier,
           int64_t size) { barrier.Run(size); },
        cache->CreateHandle(), barrier));
  }
}

void CacheStorage::Close(base::OnceClosure callback) {
  const int64_t id = scheduler_.CreateId();
  // Not through Schedule(): a storage key that was never used has nothing to
  // load or close.
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(&CacheStorage::CloseImpl, weak_factory_.GetWeakPtr(),
                     scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorage::CloseImpl(base::OnceClosure callback) {
  const base::RepeatingClosure barrier =
      base::BarrierClosure(caches_.size() + doomed_.size(), std::move(callback));
  for (auto& [name, cache] : caches_) {
    cache->Close(barrier);
  }
  for (auto& [pointer, cache] : doomed_) {
    cache->Close(barrier);
  }
}

void CacheStorage::CacheSizeUpdated(const CacheStorageCache& cache) {
  if (doomed_.contains(const_cast<CacheStorageCache*>(&cache))) {
    return;
  }
  // Only the padding is kept: the size comes from the backend when the cache
  // opens. The index is written with the next change to the list.
  for (IndexEntry& entry : index_) {
    if (entry.name == cache.cache_name()) {
      entry.padding = cache.cache_padding();
    }
  }
}

void CacheStorage::CacheUnreferenced(CacheStorageCache& cache) {
  // A cache that is still in the list stays open while the Session is, as
  // Chrome keeps one open while its CacheStorage is used.
  if (!doomed_.contains(&cache)) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&CacheStorage::FinalizeDoomedCache,
                                weak_factory_.GetWeakPtr(), &cache));
}

void CacheStorage::FinalizeDoomedCache(CacheStorageCache* cache) {
  const auto found = doomed_.find(cache);
  // A new handle may have come since, from a body that is still read.
  if (found == doomed_.end() || !cache->IsUnreferenced()) {
    return;
  }
  // DeleteCacheDidGetSize(): the quota forgets what it held, and its files
  // go once its backend has closed.
  cache->Size(base::BindOnce(
      [](base::WeakPtr<CacheStorage> self, CacheStorageCache* cache,
         int64_t size) {
        if (!self) {
          return;
        }
        if (size > 0) {
          self->quota_->NotifyModified(-size);
        }
        cache->Close(base::BindOnce(
            [](base::WeakPtr<CacheStorage> self, CacheStorageCache* cache) {
              if (!self) {
                return;
              }
              const auto found = self->doomed_.find(cache);
              if (found == self->doomed_.end()) {
                return;
              }
              const base::FilePath path = cache->path();
              // Destroyed from a task of its own, after its scheduler has
              // finished the close.
              base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                  FROM_HERE, std::move(found->second));
              self->doomed_.erase(found);
              if (!path.empty()) {
                self->file_runner_->PostTask(
                    FROM_HERE,
                    base::BindOnce(base::IgnoreResult(
                                       &base::DeletePathRecursively),
                                   path));
              }
            },
            self, cache));
      },
      weak_factory_.GetWeakPtr(), cache));
}

CacheStorageCache* CacheStorage::GetLoadedCache(
    const std::u16string& cache_name) {
  CHECK(initialized_);
  if (const auto open = caches_.find(cache_name); open != caches_.end()) {
    return open->second.get();
  }
  const auto entry = std::ranges::find_if(
      index_,
      [&cache_name](const IndexEntry& entry) { return entry.name == cache_name; });
  if (entry == index_.end()) {
    return nullptr;
  }
  std::unique_ptr<CacheStorageCache> cache = CreateCache(*entry);
  CacheStorageCache* const pointer = cache.get();
  caches_.emplace(cache_name, std::move(cache));
  return pointer;
}

std::unique_ptr<CacheStorageCache> CacheStorage::CreateCache(
    const IndexEntry& entry) {
  return std::make_unique<CacheStorageCache>(
      entry.name,
      directory_.empty() ? base::FilePath()
                         : directory_.AppendASCII(entry.directory),
      *quota_, this, entry.padding);
}

void CacheStorage::WriteIndex(base::OnceCallback<void(bool)> callback) {
  if (directory_.empty()) {
    std::move(callback).Run(true);
    return;
  }
  std::vector<std::u16string> names;
  std::vector<std::string> directories;
  std::vector<int64_t> paddings;
  for (const IndexEntry& entry : index_) {
    names.push_back(entry.name);
    directories.push_back(entry.directory);
    paddings.push_back(entry.padding);
  }
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](const base::FilePath& directory, std::string bytes) {
            return base::CreateDirectory(directory) &&
                   base::ImportantFileWriter::WriteFileAtomically(
                       directory.Append(kIndexFileName), bytes);
          },
          directory_, SerializeIndex(names, directories, paddings)),
      std::move(callback));
}

}  // namespace mitto
