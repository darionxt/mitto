#include "mitto/cache_storage/cache_storage_cache.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/barrier_closure.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/numerics/checked_math.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"

namespace mitto {
namespace {

// kMaxQueryCacheResultBytes: what one query may return, without bodies.
constexpr size_t kMaxQueryResultBytes = 10 * 1024 * 1024;

// How deep QueryOpenNextEntry() may recurse before it posts a task.
constexpr int kMaxQueryRecursiveDepth = 20;

using MetadataCallback =
    base::OnceCallback<void(std::optional<CacheMetadata>)>;

// Runs `callback` with `rv` now, unless the operation is still pending.
template <typename Result>
void RunUnlessPending(base::OnceCallback<void(Result)>& callback, Result rv) {
  if (rv != net::ERR_IO_PENDING) {
    std::move(callback).Run(rv);
  }
}

void ReadMetadata(disk_cache::Entry* entry, MetadataCallback callback) {
  const int64_t size = entry->GetDataSize(kCacheEntryHeaders);
  if (size <= 0 || size > std::numeric_limits<int>::max()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  auto buffer =
      base::MakeRefCounted<net::IOBufferWithSize>(static_cast<size_t>(size));
  auto [first, second] = base::SplitOnceCallback(base::BindOnce(
      [](scoped_refptr<net::IOBufferWithSize> buffer,
         MetadataCallback callback, int rv) {
        if (rv != buffer->size()) {
          std::move(callback).Run(std::nullopt);
          return;
        }
        std::move(callback).Run(ParseCacheMetadata(
            std::string(base::as_string_view(buffer->span()))));
      },
      buffer, std::move(callback)));
  const int rv = entry->ReadData(kCacheEntryHeaders, 0, buffer.get(),
                                 buffer->size(), std::move(first));
  RunUnlessPending(second, rv);
}

// RemoveQueryParam().
GURL RemoveQuery(const GURL& url) {
  GURL::Replacements replacements;
  replacements.ClearQuery();
  return url.ReplaceComponents(replacements);
}

// FindDuplicateOperations(): the URLs, without fragments, that two of the
// batch's operations would both write.
std::vector<std::string> FindDuplicateOperations(
    const std::vector<CacheStorageCache::BatchOperation>& operations) {
  std::vector<std::string> duplicates;
  if (operations.size() < 2) {
    return duplicates;
  }
  struct Sorted {
    raw_ptr<const CacheStorageCache::BatchOperation> operation;
    std::string url;
  };
  std::vector<Sorted> sorted;
  sorted.reserve(operations.size());
  for (const auto& operation : operations) {
    sorted.push_back({&operation, NormalizeCacheUrl(operation.request.url).spec()});
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const Sorted& left, const Sorted& right) {
              return left.url < right.url;
            });
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (!duplicates.empty() && sorted[i].url == duplicates.back()) {
      continue;
    }
    const auto& outer = *sorted[i].operation;
    for (size_t j = i + 1; j < sorted.size(); ++j) {
      if (sorted[i].url != sorted[j].url) {
        break;
      }
      const auto& inner = *sorted[j].operation;
      if (VaryMatches(outer.request.headers, inner.request.headers,
                      inner.response.response_type, inner.response.headers) ||
          VaryMatches(outer.request.headers, inner.request.headers,
                      outer.response.response_type, outer.response.headers)) {
        duplicates.push_back(sorted[j].url);
        break;
      }
    }
  }
  return duplicates;
}

// CreateRequest(): an entry's request, with its fragment back.
CacheRequest RequestFromMetadata(const CacheMetadata& metadata,
                                 const GURL& key_url) {
  CacheRequest request = metadata.request;
  if (metadata.fragment) {
    GURL::Replacements replacements;
    replacements.SetRefStr(*metadata.fragment);
    request.url = key_url.ReplaceComponents(replacements);
  } else {
    request.url = key_url;
  }
  return request;
}

}  // namespace

CacheStorageCacheHandle::CacheStorageCacheHandle() = default;

CacheStorageCacheHandle::CacheStorageCacheHandle(
    base::WeakPtr<CacheStorageCache> cache)
    : cache_(std::move(cache)) {
  if (cache_) {
    cache_->AddHandleRef();
  }
}

CacheStorageCacheHandle::CacheStorageCacheHandle(
    CacheStorageCacheHandle&& other)
    : cache_(std::move(other.cache_)) {}

CacheStorageCacheHandle& CacheStorageCacheHandle::operator=(
    CacheStorageCacheHandle&& other) {
  if (this != &other) {
    if (cache_) {
      cache_->DropHandleRef();
    }
    cache_ = std::move(other.cache_);
  }
  return *this;
}

CacheStorageCacheHandle::~CacheStorageCacheHandle() {
  if (cache_) {
    cache_->DropHandleRef();
  }
}

CacheStorageCacheHandle CacheStorageCacheHandle::Clone() const {
  return CacheStorageCacheHandle(cache_);
}

CacheStorageCache::MatchResult::MatchResult() = default;
CacheStorageCache::MatchResult::MatchResult(MatchResult&&) = default;
CacheStorageCache::MatchResult& CacheStorageCache::MatchResult::operator=(
    MatchResult&&) = default;
CacheStorageCache::MatchResult::~MatchResult() = default;

CacheStorageCache::BatchOperation::BatchOperation() = default;
CacheStorageCache::BatchOperation::BatchOperation(BatchOperation&&) = default;
CacheStorageCache::BatchOperation& CacheStorageCache::BatchOperation::operator=(
    BatchOperation&&) = default;
CacheStorageCache::BatchOperation::~BatchOperation() = default;

struct CacheStorageCache::QueryResult {
  QueryResult(base::Time entry_time, int64_t padding, int64_t side_data_padding)
      : entry_time(entry_time),
        padding(padding),
        side_data_padding(side_data_padding) {}

  std::optional<CacheRequest> request;
  std::optional<CacheResponse> response;
  disk_cache::ScopedEntryPtr entry;
  base::Time entry_time;
  int64_t padding = 0;
  int64_t side_data_padding = 0;
};

struct CacheStorageCache::QueryContext {
  QueryContext(std::optional<CacheRequest> request,
               std::optional<CacheQueryOptions> options,
               QueryCallback callback,
               uint8_t query_types)
      : request(std::move(request)),
        options(std::move(options)),
        callback(std::move(callback)),
        query_types(query_types),
        matches(std::make_unique<QueryResults>()) {}

  std::optional<CacheRequest> request;
  std::optional<CacheQueryOptions> options;
  QueryCallback callback;
  uint8_t query_types = 0;
  size_t estimated_out_bytes = 0;
  std::unique_ptr<disk_cache::Backend::Iterator> backend_iterator;
  std::unique_ptr<QueryResults> matches;
};

struct CacheStorageCache::PutContext {
  CacheRequest request;
  CacheResponse response;
  std::unique_ptr<CacheBodySource> body;
  ErrorCallback callback;
  disk_cache::ScopedEntryPtr entry;
  // How much of the body is in the entry.
  uint64_t written = 0;
};

struct CacheStorageCache::BatchInfo {
  size_t remaining_operations = 0;
  VerboseErrorCallback callback;
  std::optional<std::string> message;
};

CacheStorageCache::CacheStorageCache(std::u16string cache_name,
                                     base::FilePath path,
                                     CacheStorageQuota& quota,
                                     Observer* observer,
                                     int64_t cache_padding)
    : cache_name_(std::move(cache_name)),
      cache_name_utf8_size_(base::UTF16ToUTF8(cache_name_).size()),
      path_(std::move(path)),
      quota_(quota),
      observer_(observer),
      cache_padding_(cache_padding) {
  InitBackend();
}

CacheStorageCache::~CacheStorageCache() = default;

CacheStorageCacheHandle CacheStorageCache::CreateHandle() {
  return CacheStorageCacheHandle(weak_factory_.GetWeakPtr());
}

void CacheStorageCache::AddHandleRef() {
  ++handle_ref_count_;
}

void CacheStorageCache::DropHandleRef() {
  CHECK_GT(handle_ref_count_, 0u);
  --handle_ref_count_;
  if (handle_ref_count_ == 0 && observer_) {
    // May destroy this.
    observer_->CacheUnreferenced(*this);
  }
}

int64_t CacheStorageCache::PaddedCacheSize() const {
  if (cache_size_ == kSizeUnknown || cache_padding_ == kSizeUnknown) {
    return kSizeUnknown;
  }
  return cache_size_ + cache_padding_;
}

void CacheStorageCache::InitBackend() {
  CHECK_EQ(backend_state_, kBackendUninitialized);
  initializing_ = true;
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(
          [](base::WeakPtr<CacheStorageCache> self, base::OnceClosure done) {
            if (!self) {
              return;
            }
            // CreateBackend(): an APP_CACHE never evicts.
            const bool memory = self->path_.empty();
            auto [first, second] = base::SplitOnceCallback(
                base::BindOnce(&CacheStorageCache::InitDidCreateBackend, self,
                               std::move(done)));
            disk_cache::BackendResult result = disk_cache::CreateCacheBackend(
                memory ? net::MEMORY_CACHE : net::APP_CACHE,
                net::CACHE_BACKEND_SIMPLE, /*file_operations=*/nullptr,
                self->path_,
                memory ? std::numeric_limits<int>::max()
                       : std::numeric_limits<int64_t>::max(),
                disk_cache::ResetHandling::kNeverReset, /*net_log=*/nullptr,
                /*cache_encryption_delegate=*/nullptr,
                base::BindOnce(&CacheStorageCache::BackendCompletedIO, self),
                std::move(first));
            if (result.net_error != net::ERR_IO_PENDING) {
              std::move(second).Run(std::move(result));
            }
          },
          weak_factory_.GetWeakPtr(),
          scheduler_.WrapCallbackToRunNext(id, base::OnceClosure(
                                                   base::DoNothing()))));
}

void CacheStorageCache::InitDidCreateBackend(base::OnceClosure callback,
                                             disk_cache::BackendResult result) {
  if (result.net_error != net::OK) {
    InitGotCacheSizeAndPadding(std::move(callback), 0, 0);
    return;
  }
  backend_ = std::move(result.backend);
  auto [first, second] = base::SplitOnceCallback(base::BindOnce(
      &CacheStorageCache::InitGotCacheSize, weak_factory_.GetWeakPtr(),
      std::move(callback)));
  const int64_t rv = backend_->CalculateSizeOfAllEntries(std::move(first));
  RunUnlessPending(second, rv);
}

void CacheStorageCache::InitGotCacheSize(base::OnceClosure callback,
                                         int64_t cache_size) {
  if (cache_size < 0) {
    InitGotCacheSizeAndPadding(std::move(callback), 0, 0);
    return;
  }
  if (cache_padding_ == kSizeUnknown || cache_padding_ < 0) {
    CalculatePadding(
        cache_size,
        base::BindOnce(&CacheStorageCache::InitGotCacheSizeAndPadding,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
    return;
  }
  InitGotCacheSizeAndPadding(std::move(callback), cache_size, cache_padding_);
}

void CacheStorageCache::CalculatePadding(
    int64_t cache_size,
    base::OnceCallback<void(int64_t, int64_t)> callback) {
  CacheQueryOptions options;
  options.ignore_search = true;
  Query(CacheRequest(), options, kQueryResponsesNoBodies,
        base::BindOnce(
            [](base::OnceCallback<void(int64_t, int64_t)> callback,
               int64_t cache_size, CacheStorageError error,
               std::unique_ptr<QueryResults> results) {
              int64_t padding = 0;
              if (error == CacheStorageError::kSuccess) {
                for (const QueryResult& result : *results) {
                  padding += result.padding + result.side_data_padding;
                }
              }
              std::move(callback).Run(cache_size, padding);
            },
            std::move(callback), cache_size));
}

void CacheStorageCache::InitGotCacheSizeAndPadding(base::OnceClosure callback,
                                                   int64_t cache_size,
                                                   int64_t cache_padding) {
  cache_size_ = cache_size;
  cache_padding_ = cache_padding;
  last_reported_size_ = PaddedCacheSize();
  initializing_ = false;
  backend_state_ = backend_ && backend_state_ == kBackendUninitialized
                       ? kBackendOpen
                       : kBackendClosed;
  if (observer_) {
    observer_->CacheSizeUpdated(*this);
  }
  std::move(callback).Run();
}

void CacheStorageCache::Match(CacheRequest request,
                              std::optional<CacheQueryOptions> options,
                              MatchCallback callback) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, std::nullopt);
    return;
  }
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kShared,
      base::BindOnce(
          &CacheStorageCache::MatchAllImpl, weak_factory_.GetWeakPtr(),
          std::move(request), std::move(options),
          scheduler_.WrapCallbackToRunNext(
              id, base::BindOnce(
                      // MatchDidMatchAll(): the first, if any.
                      [](MatchCallback callback, CacheStorageError error,
                         std::vector<MatchResult> matches) {
                        if (error != CacheStorageError::kSuccess) {
                          std::move(callback).Run(error, std::nullopt);
                          return;
                        }
                        if (matches.empty()) {
                          std::move(callback).Run(
                              CacheStorageError::kErrorNotFound, std::nullopt);
                          return;
                        }
                        std::move(callback).Run(CacheStorageError::kSuccess,
                                                std::move(matches.front()));
                      },
                      std::move(callback)))));
}

void CacheStorageCache::MatchAll(std::optional<CacheRequest> request,
                                 std::optional<CacheQueryOptions> options,
                                 MatchAllCallback callback) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, {});
    return;
  }
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kShared,
      base::BindOnce(&CacheStorageCache::MatchAllImpl,
                     weak_factory_.GetWeakPtr(), std::move(request),
                     std::move(options),
                     scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorageCache::Keys(std::optional<CacheRequest> request,
                             std::optional<CacheQueryOptions> options,
                             KeysCallback callback) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, {});
    return;
  }
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kShared,
      base::BindOnce(&CacheStorageCache::KeysImpl, weak_factory_.GetWeakPtr(),
                     std::move(request), std::move(options),
                     scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorageCache::Query(std::optional<CacheRequest> request,
                              std::optional<CacheQueryOptions> options,
                              uint8_t query_types,
                              QueryCallback callback) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, nullptr);
    return;
  }
  // A method other than GET matches nothing, unless it is ignored.
  if ((!options || !options->ignore_method) && request &&
      !request->method.empty() && request->method != "GET") {
    std::move(callback).Run(CacheStorageError::kSuccess,
                            std::make_unique<QueryResults>());
    return;
  }
  std::string request_url;
  if (request) {
    request_url = NormalizeCacheUrl(request->url).spec();
  }
  auto context = std::make_unique<QueryContext>(
      std::move(request), std::move(options), std::move(callback),
      query_types);
  if (context->request && !context->request->url.is_empty() &&
      (!context->options || !context->options->ignore_search)) {
    // The fast path: the entry of that key.
    auto [first, second] = base::SplitOnceCallback(
        base::BindOnce(&CacheStorageCache::QueryDidOpenFastPath,
                       weak_factory_.GetWeakPtr(), std::move(context)));
    disk_cache::EntryResult result =
        backend_->OpenEntry(request_url, net::MEDIUM, std::move(first));
    if (result.net_error() != net::ERR_IO_PENDING) {
      std::move(second).Run(std::move(result));
    }
    return;
  }
  context->backend_iterator = backend_->CreateIterator();
  QueryOpenNextEntry(std::move(context));
}

void CacheStorageCache::QueryDidOpenFastPath(
    std::unique_ptr<QueryContext> context,
    disk_cache::EntryResult result) {
  if (result.net_error() != net::OK) {
    std::move(context->callback)
        .Run(CacheStorageError::kSuccess, std::move(context->matches));
    return;
  }
  QueryFilterEntry(std::move(context), std::move(result));
}

void CacheStorageCache::QueryOpenNextEntry(
    std::unique_ptr<QueryContext> context) {
  ++query_recursive_depth_;
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](base::WeakPtr<CacheStorageCache> self) {
        if (self) {
          --self->query_recursive_depth_;
        }
      },
      weak_factory_.GetWeakPtr()));
  if (!context->backend_iterator) {
    // Results come in the order the entries were put.
    std::stable_sort(context->matches->begin(), context->matches->end(),
                     [](const QueryResult& left, const QueryResult& right) {
                       return left.entry_time < right.entry_time;
                     });
    std::move(context->callback)
        .Run(CacheStorageError::kSuccess, std::move(context->matches));
    return;
  }
  disk_cache::Backend::Iterator& iterator = *context->backend_iterator;
  auto [first, second] = base::SplitOnceCallback(
      base::BindOnce(&CacheStorageCache::QueryFilterEntry,
                     weak_factory_.GetWeakPtr(), std::move(context)));
  disk_cache::EntryResult result = iterator.OpenNextEntry(std::move(first));
  if (result.net_error() == net::ERR_IO_PENDING) {
    return;
  }
  if (query_recursive_depth_ <= kMaxQueryRecursiveDepth) {
    std::move(second).Run(std::move(result));
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(second), std::move(result)));
}

void CacheStorageCache::QueryFilterEntry(std::unique_ptr<QueryContext> context,
                                         disk_cache::EntryResult result) {
  if (result.net_error() == net::ERR_FAILED) {
    // The end of the entries.
    context->backend_iterator.reset();
    QueryOpenNextEntry(std::move(context));
    return;
  }
  if (result.net_error() < 0) {
    std::move(context->callback)
        .Run(CacheStorageError::kErrorStorage, std::move(context->matches));
    return;
  }
  disk_cache::ScopedEntryPtr entry(result.ReleaseEntry());
  if (backend_state_ == kBackendClosed) {
    std::move(context->callback)
        .Run(CacheStorageError::kErrorNotFound, std::move(context->matches));
    return;
  }
  if (context->request && !context->request->url.is_empty()) {
    GURL request_url = NormalizeCacheUrl(context->request->url);
    GURL cached_url(entry->GetKey());
    if (context->options && context->options->ignore_search) {
      request_url = RemoveQuery(request_url);
      cached_url = RemoveQuery(cached_url);
    }
    if (cached_url != request_url) {
      QueryOpenNextEntry(std::move(context));
      return;
    }
  }
  disk_cache::Entry* const entry_ptr = entry.get();
  ReadMetadata(entry_ptr,
               base::BindOnce(&CacheStorageCache::QueryDidReadMetadata,
                              weak_factory_.GetWeakPtr(), std::move(context),
                              std::move(entry)));
}

void CacheStorageCache::QueryDidReadMetadata(
    std::unique_ptr<QueryContext> context,
    disk_cache::ScopedEntryPtr entry,
    std::optional<CacheMetadata> metadata) {
  if (!metadata) {
    entry->Doom();
    QueryOpenNextEntry(std::move(context));
    return;
  }
  QueryResult& match = context->matches->emplace_back(
      metadata->entry_time, metadata->response.padding,
      metadata->response.side_data_padding);
  match.request = RequestFromMetadata(*metadata, GURL(entry->GetKey()));
  match.response = std::move(metadata->response);
  match.response->body_size =
      static_cast<uint64_t>(entry->GetDataSize(kCacheEntryResponseBody));

  if (context->request &&
      (!context->options || !context->options->ignore_vary) &&
      !VaryMatches(context->request->headers, match.request->headers,
                   match.response->response_type, match.response->headers)) {
    context->matches->pop_back();
    QueryOpenNextEntry(std::move(context));
    return;
  }

  if (context->query_types & kQueryRequests) {
    context->estimated_out_bytes += EstimatedRequestSize(*match.request);
    if (context->estimated_out_bytes > kMaxQueryResultBytes) {
      std::move(context->callback)
          .Run(CacheStorageError::kErrorQueryTooLarge, nullptr);
      return;
    }
  } else {
    match.request.reset();
  }

  if (context->query_types & kQueryResponsesWithBodies) {
    context->estimated_out_bytes +=
        EstimatedResponseSize(*match.response, cache_name_utf8_size_);
    if (context->estimated_out_bytes > kMaxQueryResultBytes) {
      std::move(context->callback)
          .Run(CacheStorageError::kErrorQueryTooLarge, nullptr);
      return;
    }
    // A response with an empty body gets no blob, so a null body.
    if (match.response->body_size != 0) {
      match.entry = std::move(entry);
    }
  } else if (!(context->query_types & kQueryResponsesNoBodies)) {
    match.response.reset();
  }
  if (context->query_types & kQueryEntries) {
    match.entry = std::move(entry);
  }
  QueryOpenNextEntry(std::move(context));
}

void CacheStorageCache::MatchAllImpl(std::optional<CacheRequest> request,
                                     std::optional<CacheQueryOptions> options,
                                     MatchAllCallback callback) {
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, {});
    return;
  }
  Query(std::move(request), std::move(options),
        kQueryRequests | kQueryResponsesWithBodies,
        base::BindOnce(&CacheStorageCache::MatchAllDidQuery,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CacheStorageCache::MatchAllDidQuery(
    MatchAllCallback callback,
    CacheStorageError error,
    std::unique_ptr<QueryResults> results) {
  if (error != CacheStorageError::kSuccess) {
    std::move(callback).Run(error, {});
    return;
  }
  std::vector<MatchResult> matches;
  matches.reserve(results->size());
  for (QueryResult& result : *results) {
    MatchResult& match = matches.emplace_back();
    match.request = std::move(*result.request);
    match.response = std::move(*result.response);
    match.entry = std::move(result.entry);
    if (match.entry) {
      match.handle = CreateHandle();
    }
  }
  std::move(callback).Run(CacheStorageError::kSuccess, std::move(matches));
}

void CacheStorageCache::KeysImpl(std::optional<CacheRequest> request,
                                 std::optional<CacheQueryOptions> options,
                                 KeysCallback callback) {
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, {});
    return;
  }
  Query(std::move(request), std::move(options), kQueryRequests,
        base::BindOnce(
            [](KeysCallback callback, CacheStorageError error,
               std::unique_ptr<QueryResults> results) {
              if (error != CacheStorageError::kSuccess) {
                std::move(callback).Run(error, {});
                return;
              }
              std::vector<CacheRequest> requests;
              requests.reserve(results->size());
              for (QueryResult& result : *results) {
                requests.push_back(std::move(*result.request));
              }
              std::move(callback).Run(CacheStorageError::kSuccess,
                                      std::move(requests));
            },
            std::move(callback)));
}

void CacheStorageCache::Batch(
    std::vector<BatchOperation> operations,
    VerboseErrorCallback callback,
    base::OnceClosure bad_message) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage, std::nullopt);
    return;
  }
  if (const std::vector<std::string> duplicates =
          FindDuplicateOperations(operations);
      !duplicates.empty()) {
    std::move(callback).Run(
        CacheStorageError::kErrorDuplicateOperation,
        base::StringPrintf("duplicate requests (%s)",
                           base::JoinString(duplicates, ", ").c_str()));
    return;
  }
  base::CheckedNumeric<uint64_t> space_required = 0;
  size_t deletes = 0;
  for (const auto& operation : operations) {
    if (operation.type == BatchOperation::Type::kPut) {
      space_required += RequiredSpaceForResponse(operation.response);
      space_required += RequiredSpaceForRequest(operation.request);
    } else {
      ++deletes;
    }
  }
  // A delete goes alone, as the renderer sends it.
  if (!space_required.IsValid() || (deletes && operations.size() != 1)) {
    std::move(bad_message).Run();
    std::move(callback).Run(CacheStorageError::kErrorStorage, std::nullopt);
    return;
  }
  const uint64_t required = space_required.ValueOrDie();
  if (required) {
    quota_->GetSpaceRemaining(base::BindOnce(
        &CacheStorageCache::BatchDidGetSpaceRemaining,
        weak_factory_.GetWeakPtr(), std::move(operations), std::move(callback),
        std::nullopt, required));
    return;
  }
  BatchDidGetSpaceRemaining(std::move(operations), std::move(callback),
                            std::nullopt, 0, 0);
}

void CacheStorageCache::BatchDidGetSpaceRemaining(
    std::vector<BatchOperation> operations,
    VerboseErrorCallback callback,
    std::optional<std::string> message,
    uint64_t space_required,
    std::optional<int64_t> space_remaining) {
  if (backend_state_ == kBackendClosed) {
    std::move(callback).Run(CacheStorageError::kErrorStorage,
                            std::move(message));
    return;
  }
  if (!space_remaining || *space_remaining < 0 ||
      space_required > static_cast<uint64_t>(*space_remaining)) {
    std::move(callback).Run(CacheStorageError::kErrorQuotaExceeded,
                            std::move(message));
    return;
  }
  auto batch = std::make_unique<BatchInfo>();
  batch->remaining_operations = operations.size();
  batch->callback = std::move(callback);
  batch->message = std::move(message);
  const auto completion = base::BindRepeating(
      &CacheStorageCache::BatchDidOneOperation, weak_factory_.GetWeakPtr(),
      base::OwnedRef(std::move(*batch)));
  for (auto& operation : operations) {
    if (operation.type == BatchOperation::Type::kPut) {
      Put(std::move(operation), completion);
    } else {
      Delete(std::move(operation), completion);
    }
  }
}

void CacheStorageCache::BatchDidOneOperation(BatchInfo& batch,
                                             CacheStorageError error) {
  if (!batch.callback) {
    return;
  }
  CHECK_GT(batch.remaining_operations, 0u);
  --batch.remaining_operations;
  if (error != CacheStorageError::kSuccess) {
    std::move(batch.callback).Run(error, std::move(batch.message));
  } else if (batch.remaining_operations == 0) {
    std::move(batch.callback)
        .Run(CacheStorageError::kSuccess, std::move(batch.message));
  }
}

void CacheStorageCache::Put(BatchOperation operation,
                            ErrorCallback callback) {
  if (backend_state_ == kBackendClosed) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), CacheStorageError::kErrorStorage));
    return;
  }
  auto put = std::make_unique<PutContext>();
  put->request = std::move(operation.request);
  put->response = std::move(operation.response);
  put->body = std::move(operation.body);
  const int64_t id = scheduler_.CreateId();
  put->callback = scheduler_.WrapCallbackToRunNext(id, std::move(callback));
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(&CacheStorageCache::PutImpl, weak_factory_.GetWeakPtr(),
                     std::move(put)));
}

void CacheStorageCache::PutImpl(std::unique_ptr<PutContext> put) {
  if (backend_state_ != kBackendOpen) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  // The entry of the same URL goes first, whatever its method and Vary.
  CacheRequest delete_request;
  delete_request.url = put->request.url;
  CacheQueryOptions options;
  options.ignore_method = true;
  options.ignore_vary = true;
  DeleteImpl(std::move(delete_request), options,
             base::BindOnce(&CacheStorageCache::PutDidDeleteEntry,
                            weak_factory_.GetWeakPtr(), std::move(put)));
}

void CacheStorageCache::PutDidDeleteEntry(std::unique_ptr<PutContext> put,
                                          CacheStorageError error) {
  if (backend_state_ != kBackendOpen) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  if (error != CacheStorageError::kSuccess &&
      error != CacheStorageError::kErrorNotFound) {
    PutComplete(std::move(put), error);
    return;
  }
  const std::string key = NormalizeCacheUrl(put->request.url).spec();
  auto [first, second] = base::SplitOnceCallback(
      base::BindOnce(&CacheStorageCache::PutDidCreateEntry,
                     weak_factory_.GetWeakPtr(), std::move(put)));
  disk_cache::EntryResult result =
      backend_->OpenOrCreateEntry(key, net::MEDIUM, std::move(first));
  if (result.net_error() != net::ERR_IO_PENDING) {
    std::move(second).Run(std::move(result));
  }
}

void CacheStorageCache::PutDidCreateEntry(std::unique_ptr<PutContext> put,
                                          disk_cache::EntryResult result) {
  const int rv = result.net_error();
  put->entry.reset(result.ReleaseEntry());
  if (rv != net::OK) {
    PutComplete(std::move(put), CacheStorageError::kErrorExists);
    return;
  }
  CacheMetadata metadata;
  metadata.entry_time = base::Time::Now();
  metadata.request = put->request;
  metadata.request.url = GURL();
  if (put->request.url.has_ref()) {
    metadata.fragment = std::string(put->request.url.ref());
  }
  metadata.response = put->response;
  // Side data comes only from a service worker, which a Page has none of.
  metadata.response.side_data_padding = 0;
  const std::string serialized = SerializeCacheMetadata(metadata);
  auto buffer = base::MakeRefCounted<net::StringIOBuffer>(serialized);
  const int expected = buffer->size();
  disk_cache::Entry* const entry = put->entry.get();
  auto [first, second] = base::SplitOnceCallback(
      base::BindOnce(&CacheStorageCache::PutDidWriteHeaders,
                     weak_factory_.GetWeakPtr(), std::move(put), expected));
  const int write_rv =
      entry->WriteData(kCacheEntryHeaders, 0, buffer.get(), expected,
                       std::move(first), /*truncate=*/true);
  RunUnlessPending(second, write_rv);
}

void CacheStorageCache::PutDidWriteHeaders(std::unique_ptr<PutContext> put,
                                           int expected_bytes,
                                           int rv) {
  if (rv != expected_bytes) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  if (ShouldPadResponseType(put->response.response_type)) {
    cache_padding_ += put->response.padding;
  }
  PutReadBody(std::move(put));
}

void CacheStorageCache::PutReadBody(std::unique_ptr<PutContext> put) {
  if (put->written == put->response.body_size) {
    // Each stream that it does not write is left empty, as a put of no body
    // truncates its stream to nothing.
    disk_cache::Entry* const entry = put->entry.get();
    if (entry->GetDataSize(kCacheEntryResponseBody) != 0 ||
        put->response.body_size != 0) {
      PutComplete(std::move(put), CacheStorageError::kSuccess);
      return;
    }
    auto [first, second] = base::SplitOnceCallback(
        base::BindOnce(&CacheStorageCache::PutDidWriteBody,
                       weak_factory_.GetWeakPtr(), std::move(put), 0));
    const int rv = entry->WriteData(kCacheEntryResponseBody, 0, nullptr, 0,
                                    std::move(first), /*truncate=*/true);
    RunUnlessPending(second, rv);
    return;
  }
  if (!put->body) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  CacheBodySource* const body = put->body.get();
  body->ReadNext(base::BindOnce(&CacheStorageCache::PutDidReadBody,
                                weak_factory_.GetWeakPtr(), std::move(put)));
}

void CacheStorageCache::PutDidReadBody(std::unique_ptr<PutContext> put,
                                       std::optional<std::string> bytes) {
  if (!bytes || bytes->empty() ||
      bytes->size() > put->response.body_size - put->written) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  auto buffer = base::MakeRefCounted<net::StringIOBuffer>(std::move(*bytes));
  const int expected = buffer->size();
  const int64_t offset = static_cast<int64_t>(put->written);
  const bool last = put->written + expected == put->response.body_size;
  disk_cache::Entry* const entry = put->entry.get();
  auto [first, second] = base::SplitOnceCallback(
      base::BindOnce(&CacheStorageCache::PutDidWriteBody,
                     weak_factory_.GetWeakPtr(), std::move(put), expected));
  const int rv = entry->WriteData(kCacheEntryResponseBody, offset, buffer.get(),
                                  expected, std::move(first), last);
  RunUnlessPending(second, rv);
}

void CacheStorageCache::PutDidWriteBody(std::unique_ptr<PutContext> put,
                                        int expected_bytes,
                                        int rv) {
  if (rv != expected_bytes) {
    PutComplete(std::move(put), CacheStorageError::kErrorStorage);
    return;
  }
  if (expected_bytes == 0) {
    PutComplete(std::move(put), CacheStorageError::kSuccess);
    return;
  }
  put->written += static_cast<uint64_t>(expected_bytes);
  PutReadBody(std::move(put));
}

void CacheStorageCache::PutComplete(std::unique_ptr<PutContext> put,
                                    CacheStorageError error) {
  // ScopedWritableEntry: an entry that was not written whole goes.
  if (error != CacheStorageError::kSuccess && put->entry) {
    put->entry->Doom();
  }
  put->entry.reset();
  put->body.reset();
  ErrorCallback callback = std::move(put->callback);
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run(error);
    return;
  }
  UpdateCacheSize(base::BindOnce(std::move(callback), error));
}

void CacheStorageCache::Delete(BatchOperation operation,
                               ErrorCallback callback) {
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(&CacheStorageCache::DeleteImpl, weak_factory_.GetWeakPtr(),
                     std::move(operation.request),
                     std::move(operation.match_options),
                     scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorageCache::DeleteImpl(CacheRequest request,
                                   std::optional<CacheQueryOptions> options,
                                   ErrorCallback callback) {
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run(CacheStorageError::kErrorStorage);
    return;
  }
  Query(std::move(request), std::move(options),
        kQueryEntries | kQueryResponsesNoBodies,
        base::BindOnce(&CacheStorageCache::DeleteDidQuery,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CacheStorageCache::DeleteDidQuery(ErrorCallback callback,
                                       CacheStorageError error,
                                       std::unique_ptr<QueryResults> results) {
  if (error != CacheStorageError::kSuccess) {
    std::move(callback).Run(error);
    return;
  }
  if (results->empty()) {
    std::move(callback).Run(CacheStorageError::kErrorNotFound);
    return;
  }
  for (QueryResult& result : *results) {
    if (ShouldPadResponseType(result.response->response_type)) {
      cache_padding_ -= result.padding + result.side_data_padding;
    }
    result.entry->Doom();
  }
  UpdateCacheSize(base::BindOnce(std::move(callback),
                                 CacheStorageError::kSuccess));
}

void CacheStorageCache::UpdateCacheSize(base::OnceClosure callback) {
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run();
    return;
  }
  auto [first, second] = base::SplitOnceCallback(base::BindOnce(
      &CacheStorageCache::UpdateCacheSizeGotSize, weak_factory_.GetWeakPtr(),
      std::move(callback)));
  const int64_t rv = backend_->CalculateSizeOfAllEntries(std::move(first));
  RunUnlessPending(second, rv);
}

void CacheStorageCache::UpdateCacheSizeGotSize(base::OnceClosure callback,
                                               int64_t current_cache_size) {
  if (current_cache_size >= 0) {
    cache_size_ = current_cache_size;
  }
  const int64_t delta = PaddedCacheSize() - last_reported_size_;
  last_reported_size_ = PaddedCacheSize();
  if (delta) {
    quota_->NotifyModified(delta);
  }
  if (observer_) {
    observer_->CacheSizeUpdated(*this);
  }
  std::move(callback).Run();
}

void CacheStorageCache::Size(SizeCallback callback) {
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kShared,
      base::BindOnce(
          [](base::WeakPtr<CacheStorageCache> self, SizeCallback callback) {
            if (!self) {
              return;
            }
            std::move(callback).Run(self->backend_state_ == kBackendOpen
                                        ? self->PaddedCacheSize()
                                        : 0);
          },
          weak_factory_.GetWeakPtr(),
          scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorageCache::Close(base::OnceClosure callback) {
  const int64_t id = scheduler_.CreateId();
  scheduler_.ScheduleOperation(
      id, CacheStorageScheduler::Mode::kExclusive,
      base::BindOnce(&CacheStorageCache::CloseImpl, weak_factory_.GetWeakPtr(),
                     scheduler_.WrapCallbackToRunNext(id, std::move(callback))));
}

void CacheStorageCache::CloseImpl(base::OnceClosure callback) {
  if (backend_state_ != kBackendOpen) {
    std::move(callback).Run();
    return;
  }
  backend_closed_callback_ = std::move(callback);
  backend_.reset();
}

void CacheStorageCache::BackendCompletedIO() {
  backend_state_ = kBackendClosed;
  if (backend_closed_callback_) {
    std::move(backend_closed_callback_).Run();
  }
}

}  // namespace mitto
