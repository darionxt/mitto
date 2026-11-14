#include "mitto/page_cache_storage.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "mitto/cache_storage/cache_storage.h"
#include "mitto/cache_storage/cache_storage_context.h"
#include "mitto/page_messages.h"
#include "mitto/storage_quota.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"

namespace mitto {
namespace {

// How many bytes of a put's body the library asks the Page for at once.
constexpr uint32_t kPutBodyPartSize = 1024 * 1024;

// How many bytes of a stored body the library sends before the Page gives
// credit for them, as for a response's body.
constexpr uint32_t kStoredBodyWindow = kBodyWindow;

CacheRequest ToCacheRequest(const PageCacheRequest& request) {
  CacheRequest cache_request;
  cache_request.url = GURL(request.url);
  cache_request.method = request.method;
  cache_request.headers = CacheHeaderMap(request.headers);
  return cache_request;
}

PageCacheRequest ToPageRequest(const CacheRequest& request) {
  PageCacheRequest page_request;
  page_request.url = request.url.spec();
  page_request.method = request.method;
  page_request.headers.assign(request.headers.begin(), request.headers.end());
  return page_request;
}

CacheQueryOptions ToCacheOptions(const PageCacheQueryOptions& options) {
  CacheQueryOptions cache_options;
  cache_options.ignore_search = options.ignore_search;
  cache_options.ignore_method = options.ignore_method;
  cache_options.ignore_vary = options.ignore_vary;
  return cache_options;
}

CacheResponse ToCacheResponse(const PageCacheResponse& response) {
  CacheResponse cache_response;
  for (const std::string& url : response.url_list) {
    cache_response.url_list.emplace_back(url);
  }
  cache_response.status_code = response.status;
  cache_response.status_text = response.status_text;
  cache_response.response_type = static_cast<CacheResponseType>(response.type);
  cache_response.headers = CacheHeaderMap(response.headers);
  cache_response.cors_exposed_header_names =
      response.cors_exposed_header_names;
  cache_response.response_time = base::Time::FromDeltaSinceWindowsEpoch(
      base::Microseconds(response.response_time));
  cache_response.body_size = response.body_size;
  return cache_response;
}

PageCacheResponse ToPageResponse(const CacheResponse& response) {
  PageCacheResponse page_response;
  for (const GURL& url : response.url_list) {
    page_response.url_list.push_back(url.spec());
  }
  page_response.status = static_cast<uint16_t>(
      std::clamp<int32_t>(response.status_code, 0, 0xffff));
  page_response.status_text = response.status_text;
  page_response.type = static_cast<uint8_t>(response.response_type);
  page_response.headers.assign(response.headers.begin(),
                               response.headers.end());
  page_response.cors_exposed_header_names =
      response.cors_exposed_header_names;
  page_response.response_time =
      response.response_time.ToDeltaSinceWindowsEpoch().InMicroseconds();
  return page_response;
}

// What Blink's Cache refuses to put, which a Page checks before it sends: a
// request that is not a GET of an HTTP or HTTPS URL, a response whose Vary
// says *, and a partial one, unless it is opaque.
bool IsPutValid(const PageCacheOperation& operation) {
  const GURL url(operation.request.url);
  if (!url.SchemeIsHTTPOrHTTPS() || operation.request.method != "GET") {
    return false;
  }
  const PageCacheResponse& response = operation.response;
  const bool opaque =
      response.type == static_cast<uint8_t>(CacheResponseType::kOpaque) ||
      response.type ==
          static_cast<uint8_t>(CacheResponseType::kOpaqueRedirect);
  if (!opaque && response.status == 206) {
    return false;
  }
  for (const auto& [name, value] : response.headers) {
    if (base::EqualsCaseInsensitiveASCII(name, "vary")) {
      for (std::string_view field :
           base::SplitStringPiece(value, ",", base::TRIM_WHITESPACE,
                                  base::SPLIT_WANT_NONEMPTY)) {
        if (field == "*") {
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace

// The body of a put, which comes from the Page as the cache asks for it.
class PageCacheStorage::PutBody final : public CacheBodySource {
 public:
  PutBody(base::WeakPtr<PageCacheStorage> host,
          uint64_t body,
          uint64_t size,
          uint64_t context)
      : host_(std::move(host)),
        body_(body),
        remaining_(size),
        context_(context) {}

  ~PutBody() override {
    if (host_) {
      host_->ForgetPutBody(body_);
    }
  }

  uint64_t body() const { return body_; }
  // The Page's context that put it.
  uint64_t context() const { return context_; }
  bool waiting() const { return !done_.is_null(); }
  uint32_t wanted() const { return wanted_; }

  // CacheBodySource:
  void ReadNext(
      base::OnceCallback<void(std::optional<std::string>)> done) override {
    if (!host_ || remaining_ == 0) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(done), std::nullopt));
      return;
    }
    done_ = std::move(done);
    wanted_ = static_cast<uint32_t>(
        std::min<uint64_t>(remaining_, kPutBodyPartSize));
    host_->WantBody(*this, wanted_);
  }

  // The part the Page sent, which is never more than was asked for.
  void Arrived(std::string bytes) {
    remaining_ -= bytes.size();
    wanted_ = 0;
    std::move(done_).Run(std::move(bytes));
  }

  // The Page has gone, or the context that put it.
  void Failed() {
    host_.reset();
    if (done_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(done_), std::nullopt));
    }
  }

 private:
  base::WeakPtr<PageCacheStorage> host_;
  const uint64_t body_;
  uint64_t remaining_;
  const uint64_t context_;
  uint32_t wanted_ = 0;
  base::OnceCallback<void(std::optional<std::string>)> done_;
};

// A stored body that the Page may read, which keeps its entry, and so its
// cache, open until the Page has read it or no longer needs it.
struct PageCacheStorage::StoredBody {
  disk_cache::ScopedEntryPtr entry;
  CacheStorageCacheHandle handle;
  uint64_t size = 0;
  uint64_t offset = 0;
  bool started = false;
  bool reading = false;
  // Bytes sent that the Page has not given credit back for.
  uint32_t unacknowledged = 0;
  scoped_refptr<net::IOBufferWithSize> buffer;
};

PageCacheStorage::PageCacheStorage(CacheStorageContext& context,
                                   StorageQuota& quota,
                                   Send send,
                                   Refuse refuse,
                                   uint64_t first_number)
    : context_(context),
      quota_(quota),
      send_(std::move(send)),
      refuse_(std::move(refuse)),
      next_cache_(first_number),
      next_stored_body_(first_number) {}

uint64_t PageCacheStorage::NextNumber() const {
  return std::max(next_cache_, next_stored_body_);
}

void PageCacheStorage::ForgetContexts(const std::set<uint64_t>& contexts) {
  // Each fails from a task of its own, and no longer counts as the Page's.
  for (auto it = put_bodies_.begin(); it != put_bodies_.end();) {
    if (!contexts.contains(it->second->context())) {
      ++it;
      continue;
    }
    it->second->Failed();
    it = put_bodies_.erase(it);
  }
}

PageCacheStorage::~PageCacheStorage() {
  // Nothing that answers may reach this from here on.
  weak_factory_.InvalidateWeakPtrs();
  // The puts that wait for a body fail, from a task of their own.
  std::map<uint64_t, PutBody*> put_bodies = std::move(put_bodies_);
  for (auto& [body, put_body] : put_bodies) {
    put_body->Failed();
  }
}

bool PageCacheStorage::Admit(uint64_t question,
                             const std::optional<std::string>& storage_key) {
  if (question <= last_question_) {
    refuse_.Run(base::StrCat({"a Cache API question numbered ",
                              base::NumberToString(question),
                              ", not larger than the one before"}));
    return false;
  }
  last_question_ = question;
  if (!storage_key) {
    // Blink throws a SecurityError before it asks, for a document whose
    // origin may not reach the Cache API.
    refuse_.Run(
        "a Cache API call from a document whose origin may not reach caches");
    return false;
  }
  return true;
}

CacheStorageCache* PageCacheStorage::FindCache(uint64_t cache) {
  const auto found = caches_.find(cache);
  if (found == caches_.end() || !found->second.value()) {
    refuse_.Run(base::StrCat({"a Cache API call of cache ",
                              base::NumberToString(cache),
                              ", which the Page has not opened"}));
    return nullptr;
  }
  return found->second.value();
}

void PageCacheStorage::OnCall(PageCacheStorageCall call,
                              uint64_t context,
                              const std::optional<std::string>& storage_key) {
  switch (call.op) {
    case CacheStorageCallOp::kBodyPart:
      OnBodyPart(std::move(call));
      return;
    case CacheStorageCallOp::kReadBody:
      OnReadBody(call.body);
      return;
    case CacheStorageCallOp::kBodyCredit:
      OnBodyCredit(call.body, call.credit);
      return;
    case CacheStorageCallOp::kReleaseBody:
      // One may be released as its end is on the way to the Page.
      stored_bodies_.erase(call.body);
      return;
    case CacheStorageCallOp::kReleaseCache:
      if (caches_.erase(call.cache) == 0) {
        refuse_.Run(base::StrCat({"a release of cache ",
                                  base::NumberToString(call.cache),
                                  ", which the Page has not opened"}));
      }
      return;
    default:
      break;
  }
  if (!Admit(call.question, storage_key)) {
    return;
  }
  const uint64_t question = call.question;
  const base::WeakPtr<PageCacheStorage> self = weak_factory_.GetWeakPtr();
  switch (call.op) {
    case CacheStorageCallOp::kOpen:
      OnOpen(std::move(call), *storage_key);
      return;
    case CacheStorageCallOp::kHas:
    case CacheStorageCallOp::kDelete: {
      auto answer = base::BindOnce(
          [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
             bool value, CacheStorageError error) {
            if (!self) {
              return;
            }
            PageCacheStorageAnswer answer;
            answer.op = CacheStorageAnswerOp::kBoolean;
            answer.question = question;
            answer.error = static_cast<uint8_t>(error);
            answer.value = value;
            self->SendAnswer(answer);
          },
          self, question);
      CacheStorage& caches = context_->ForKey(*storage_key);
      if (call.op == CacheStorageCallOp::kHas) {
        // CacheStorageDispatcherHost: a cache that is not there answers
        // kErrorNotFound.
        caches.HasCache(
            std::move(call.cache_name),
            base::BindOnce(
                [](base::OnceCallback<void(bool, CacheStorageError)> answer,
                   bool has, CacheStorageError error) {
                  if (!has && error == CacheStorageError::kSuccess) {
                    error = CacheStorageError::kErrorNotFound;
                  }
                  std::move(answer).Run(has, error);
                },
                std::move(answer)));
      } else {
        caches.DoomCache(
            std::move(call.cache_name),
            base::BindOnce(
                [](base::OnceCallback<void(bool, CacheStorageError)> answer,
                   CacheStorageError error) {
                  std::move(answer).Run(error == CacheStorageError::kSuccess,
                                        error);
                },
                std::move(answer)));
      }
      return;
    }
    case CacheStorageCallOp::kKeys:
      context_->ForKey(*storage_key)
          .EnumerateCaches(base::BindOnce(
              [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
                 std::vector<std::u16string> names) {
                if (!self) {
                  return;
                }
                PageCacheStorageAnswer answer;
                answer.op = CacheStorageAnswerOp::kNames;
                answer.question = question;
                answer.names = std::move(names);
                self->SendAnswer(answer);
              },
              self, question));
      return;
    case CacheStorageCallOp::kMatch:
      OnMatch(std::move(call), *storage_key);
      return;
    case CacheStorageCallOp::kEstimate:
      quota_->GetUsageAndQuota(
          *storage_key,
          base::BindOnce(
              [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
                 StorageQuota::UsageAndQuota usage) {
                if (!self) {
                  return;
                }
                PageCacheStorageAnswer answer;
                answer.op = CacheStorageAnswerOp::kEstimated;
                answer.question = question;
                answer.usage = usage.usage;
                answer.quota = usage.reported_quota;
                answer.indexed_db = usage.indexed_db;
                answer.caches = usage.caches;
                self->SendAnswer(answer);
              },
              self, question));
      return;
    case CacheStorageCallOp::kCacheMatch:
    case CacheStorageCallOp::kCacheMatchAll: {
      CacheStorageCache* const cache = FindCache(call.cache);
      if (!cache) {
        return;
      }
      std::optional<CacheRequest> request;
      if (call.request) {
        request = ToCacheRequest(*call.request);
      }
      if (call.op == CacheStorageCallOp::kCacheMatch) {
        cache->Match(
            std::move(*request), ToCacheOptions(call.options),
            base::BindOnce(
                [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
                   CacheStorageError error,
                   std::optional<CacheStorageCache::MatchResult> match) {
                  if (!self) {
                    return;
                  }
                  std::vector<CacheStorageCache::MatchResult> matches;
                  if (match) {
                    matches.push_back(std::move(*match));
                  }
                  self->AnswerResponses(question, error, std::move(matches));
                },
                self, question));
      } else {
        cache->MatchAll(
            std::move(request), ToCacheOptions(call.options),
            base::BindOnce(&PageCacheStorage::AnswerResponses, self, question));
      }
      return;
    }
    case CacheStorageCallOp::kCacheKeys: {
      CacheStorageCache* const cache = FindCache(call.cache);
      if (!cache) {
        return;
      }
      std::optional<CacheRequest> request;
      if (call.request) {
        request = ToCacheRequest(*call.request);
      }
      cache->Keys(
          std::move(request), ToCacheOptions(call.options),
          base::BindOnce(
              [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
                 CacheStorageError error, std::vector<CacheRequest> requests) {
                if (!self) {
                  return;
                }
                PageCacheStorageAnswer answer;
                answer.op = CacheStorageAnswerOp::kRequests;
                answer.question = question;
                answer.error = static_cast<uint8_t>(error);
                for (const CacheRequest& request : requests) {
                  answer.requests.push_back(ToPageRequest(request));
                }
                self->SendAnswer(answer);
              },
              self, question));
      return;
    }
    case CacheStorageCallOp::kCacheBatch: {
      CacheStorageCache* const cache = FindCache(call.cache);
      if (!cache) {
        return;
      }
      OnCacheBatch(std::move(call), context, *cache);
      return;
    }
    case CacheStorageCallOp::kBodyPart:
    case CacheStorageCallOp::kReadBody:
    case CacheStorageCallOp::kBodyCredit:
    case CacheStorageCallOp::kReleaseBody:
    case CacheStorageCallOp::kReleaseCache:
      NOTREACHED();
  }
}

void PageCacheStorage::OnOpen(PageCacheStorageCall call,
                              const std::string& storage_key) {
  context_->ForKey(storage_key)
      .OpenCache(std::move(call.cache_name),
                 base::BindOnce(
                     [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
                        CacheStorageCacheHandle handle,
                        CacheStorageError error) {
                       if (!self) {
                         return;
                       }
                       PageCacheStorageAnswer answer;
                       answer.op = CacheStorageAnswerOp::kOpened;
                       answer.question = question;
                       answer.error = static_cast<uint8_t>(error);
                       if (error == CacheStorageError::kSuccess) {
                         answer.cache = self->next_cache_++;
                         self->caches_.emplace(answer.cache, std::move(handle));
                       }
                       self->SendAnswer(answer);
                     },
                     weak_factory_.GetWeakPtr(), call.question));
}

void PageCacheStorage::OnMatch(PageCacheStorageCall call,
                               const std::string& storage_key) {
  auto answer = base::BindOnce(
      [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
         CacheStorageError error,
         std::optional<CacheStorageCache::MatchResult> match) {
        if (!self) {
          return;
        }
        std::vector<CacheStorageCache::MatchResult> matches;
        if (match) {
          matches.push_back(std::move(*match));
        }
        self->AnswerResponses(question, error, std::move(matches));
      },
      weak_factory_.GetWeakPtr(), call.question);
  CacheStorage& caches = context_->ForKey(storage_key);
  CacheRequest request = ToCacheRequest(*call.request);
  if (call.has_cache_name) {
    caches.MatchCache(std::move(call.cache_name), std::move(request),
                      ToCacheOptions(call.options), std::move(answer));
  } else {
    caches.MatchAllCaches(std::move(request), ToCacheOptions(call.options),
                          std::move(answer));
  }
}

void PageCacheStorage::OnCacheBatch(PageCacheStorageCall call,
                                    uint64_t context,
                                    CacheStorageCache& cache) {
  std::vector<CacheStorageCache::BatchOperation> operations;
  for (PageCacheOperation& operation : call.operations) {
    CacheStorageCache::BatchOperation& batch_operation =
        operations.emplace_back();
    batch_operation.request = ToCacheRequest(operation.request);
    if (!operation.put) {
      batch_operation.type = CacheStorageCache::BatchOperation::Type::kDelete;
      batch_operation.match_options = ToCacheOptions(operation.options);
      continue;
    }
    if (!IsPutValid(operation)) {
      refuse_.Run("a Cache put that Blink would not have sent");
      return;
    }
    batch_operation.response = ToCacheResponse(operation.response);
    const uint64_t body = operation.response.body;
    if (body != 0) {
      if (put_bodies_.contains(body)) {
        refuse_.Run(base::StrCat({"a second Cache put of body ",
                                  base::NumberToString(body)}));
        return;
      }
      auto put_body =
          std::make_unique<PutBody>(weak_factory_.GetWeakPtr(), body,
                                    operation.response.body_size, context);
      put_bodies_.emplace(body, put_body.get());
      batch_operation.body = std::move(put_body);
    }
  }
  cache.Batch(
      std::move(operations),
      base::BindOnce(
          [](base::WeakPtr<PageCacheStorage> self, uint64_t question,
             CacheStorageError error, std::optional<std::string> message) {
            if (!self) {
              return;
            }
            PageCacheStorageAnswer answer;
            answer.op = CacheStorageAnswerOp::kBatchDone;
            answer.question = question;
            answer.error = static_cast<uint8_t>(error);
            answer.message = std::move(message);
            self->SendAnswer(answer);
          },
          weak_factory_.GetWeakPtr(), call.question),
      base::BindOnce(refuse_, "a Cache batch that no renderer sends"));
}

void PageCacheStorage::WantBody(PutBody& put_body, uint32_t most) {
  PageCacheStorageAnswer answer;
  answer.op = CacheStorageAnswerOp::kBodyWanted;
  answer.body = put_body.body();
  answer.most = most;
  SendAnswer(answer);
}

void PageCacheStorage::ForgetPutBody(uint64_t body) {
  put_bodies_.erase(body);
}

void PageCacheStorage::OnBodyPart(PageCacheStorageCall call) {
  const auto found = put_bodies_.find(call.body);
  if (found == put_bodies_.end() || !found->second->waiting() ||
      call.bytes.size() > found->second->wanted()) {
    refuse_.Run(base::StrCat({"a part of Cache put body ",
                              base::NumberToString(call.body),
                              " that the library did not ask for"}));
    return;
  }
  found->second->Arrived(std::move(call.bytes));
}

void PageCacheStorage::AnswerResponses(
    uint64_t question,
    CacheStorageError error,
    std::vector<CacheStorageCache::MatchResult> matches) {
  PageCacheStorageAnswer answer;
  answer.op = CacheStorageAnswerOp::kResponses;
  answer.question = question;
  answer.error = static_cast<uint8_t>(error);
  std::vector<uint64_t> bodies;
  for (CacheStorageCache::MatchResult& match : matches) {
    PageCacheResponse response = ToPageResponse(match.response);
    if (match.entry && match.response.body_size != 0) {
      auto stored = std::make_unique<StoredBody>();
      stored->entry = std::move(match.entry);
      stored->handle = std::move(match.handle);
      stored->size = match.response.body_size;
      response.body = next_stored_body_++;
      response.body_size = stored->size;
      bodies.push_back(response.body);
      stored_bodies_.emplace(response.body, std::move(stored));
    }
    answer.responses.push_back(std::move(response));
  }
  if (!CacheStorageAnswerFits(answer)) {
    for (uint64_t body : bodies) {
      stored_bodies_.erase(body);
    }
    answer.responses.clear();
    answer.error = static_cast<uint8_t>(CacheStorageError::kErrorQueryTooLarge);
  }
  SendAnswer(answer);
}

void PageCacheStorage::SendAnswer(const PageCacheStorageAnswer& answer) {
  if (CacheStorageAnswerFits(answer)) {
    send_.Run(EncodeFrame(PageMessage::kCacheStorageAnswer,
                          EncodeCacheStorageAnswer(answer)));
    return;
  }
  // Only a list too long for a frame gets here: what the Page hears instead
  // is the error of a query too large, or no names.
  PageCacheStorageAnswer short_answer;
  short_answer.op = answer.op;
  short_answer.question = answer.question;
  short_answer.error =
      static_cast<uint8_t>(CacheStorageError::kErrorQueryTooLarge);
  send_.Run(EncodeFrame(PageMessage::kCacheStorageAnswer,
                        EncodeCacheStorageAnswer(short_answer)));
}

void PageCacheStorage::OnReadBody(uint64_t body) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end() || found->second->started) {
    refuse_.Run(base::StrCat({"a read of stored body ",
                              base::NumberToString(body),
                              ", which the library did not give the Page"}));
    return;
  }
  found->second->started = true;
  ReadMore(body);
}

void PageCacheStorage::OnBodyCredit(uint64_t body, uint32_t credit) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end()) {
    // Credit may come for a body whose end is on the way to the Page.
    return;
  }
  StoredBody& stored = *found->second;
  if (!stored.started || credit > stored.unacknowledged) {
    refuse_.Run(base::StrCat({"credit for more of stored body ",
                              base::NumberToString(body),
                              " than the library sent"}));
    return;
  }
  stored.unacknowledged -= credit;
  ReadMore(body);
}

void PageCacheStorage::ReadMore(uint64_t body) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end()) {
    return;
  }
  StoredBody& stored = *found->second;
  if (stored.reading) {
    return;
  }
  if (stored.offset == stored.size) {
    PageCacheStorageAnswer answer;
    answer.op = CacheStorageAnswerOp::kBodyEnd;
    answer.body = body;
    answer.net_error = 0;
    stored_bodies_.erase(found);
    SendAnswer(answer);
    return;
  }
  const uint32_t room = kStoredBodyWindow - stored.unacknowledged;
  if (room == 0) {
    return;
  }
  const size_t size = static_cast<size_t>(
      std::min<uint64_t>(room, stored.size - stored.offset));
  stored.buffer = base::MakeRefCounted<net::IOBufferWithSize>(size);
  stored.reading = true;
  auto [first, second] = base::SplitOnceCallback(base::BindOnce(
      &PageCacheStorage::DidRead, weak_factory_.GetWeakPtr(), body));
  const int rv = stored.entry->ReadData(
      kCacheEntryResponseBody, static_cast<int64_t>(stored.offset),
      stored.buffer.get(), static_cast<int>(size), std::move(first));
  if (rv != net::ERR_IO_PENDING) {
    std::move(second).Run(rv);
  }
}

void PageCacheStorage::DidRead(uint64_t body, int rv) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end()) {
    return;
  }
  StoredBody& stored = *found->second;
  stored.reading = false;
  if (rv <= 0) {
    PageCacheStorageAnswer answer;
    answer.op = CacheStorageAnswerOp::kBodyEnd;
    answer.body = body;
    answer.net_error = rv < 0 ? rv : net::ERR_FAILED;
    stored_bodies_.erase(found);
    SendAnswer(answer);
    return;
  }
  PageCacheStorageAnswer answer;
  answer.op = CacheStorageAnswerOp::kBodyData;
  answer.body = body;
  answer.bytes.assign(base::as_string_view(stored.buffer->first(
      static_cast<size_t>(rv))));
  stored.buffer.reset();
  stored.offset += static_cast<uint64_t>(rv);
  stored.unacknowledged += static_cast<uint32_t>(rv);
  SendAnswer(answer);
  ReadMore(body);
}

}  // namespace mitto
