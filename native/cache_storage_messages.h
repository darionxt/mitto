// The messages of the Cache API between a Page and its Session: what the
// Page's CacheStorage and Cache objects ask, as Blink asks Chrome's browser
// over blink.mojom.CacheStorage and CacheStorageCache, and what the Session
// answers, and estimate() of the quota that the caches count toward, as
// blink.mojom.QuotaManagerHost answers it. Two types carry them, kCacheStorage to the Session and
// kCacheStorageAnswer to the Page, each with an operation first. The library
// reads what a Page sends as hostile input, so each payload is checked here
// before it acts on it.
//
// Bodies travel apart, so that neither side holds more of one than it asked
// for: a put's body goes to the Session as it asks for it, a part at a time,
// and a stored body comes to the Page as the Page gives credit for it, as a
// response's body does.
//
// docs/runtime.md describes each message.

#ifndef MITTO_CACHE_STORAGE_MESSAGES_H_
#define MITTO_CACHE_STORAGE_MESSAGES_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/types/expected.h"

namespace mitto {

// What a Page asks of its Session's Cache API.
enum class CacheStorageCallOp : uint8_t {
  // CacheStorage.open(), has(), delete(), keys() and match().
  kOpen = 1,
  kHas = 2,
  kDelete = 3,
  kKeys = 4,
  kMatch = 5,
  // A Cache's match(), matchAll(), keys(), and its puts and delete.
  kCacheMatch = 6,
  kCacheMatchAll = 7,
  kCacheKeys = 8,
  kCacheBatch = 9,
  // The next part of a put's body, which the Session asked for.
  kBodyPart = 10,
  // Starts to send a stored body, and gives credit for more of it.
  kReadBody = 11,
  kBodyCredit = 12,
  // The Page no longer needs a stored body, or a cache.
  kReleaseBody = 13,
  kReleaseCache = 14,
  // navigator.storage.estimate(): what the document's storage key holds, of
  // the Cache API and IndexedDB, and its quota.
  kEstimate = 15,
  kMaxValue = kEstimate,
};

// What the Session answers.
enum class CacheStorageAnswerOp : uint8_t {
  // To kOpen: an error, and the cache's number.
  kOpened = 1,
  // To kHas and kDelete: an error, and a byte.
  kBoolean = 2,
  // To kKeys: the names of the caches.
  kNames = 3,
  // To kMatch, kCacheMatch and kCacheMatchAll: an error, and the responses.
  kResponses = 4,
  // To kCacheKeys: an error, and the requests.
  kRequests = 5,
  // To kCacheBatch: an error, and a message if it has one.
  kBatchDone = 6,
  // Asks for the next part of a put's body, at most so many bytes.
  kBodyWanted = 7,
  // The next bytes of a stored body, and its end.
  kBodyData = 8,
  kBodyEnd = 9,
  // To kEstimate: the usage, the quota that estimate() reports, and the usage
  // of IndexedDB and of the caches, each a 64-bit number.
  kEstimated = 10,
  kMaxValue = kEstimated,
};

// blink.mojom.CacheQueryOptions.
struct PageCacheQueryOptions {
  bool ignore_search = false;
  bool ignore_method = false;
  bool ignore_vary = false;
};

// What a Page sends of a request: blink.mojom.FetchAPIRequest's URL, method
// and header lines, each name once, as CreateFetchAPIRequest() joins them.
struct PageCacheRequest {
  PageCacheRequest();
  PageCacheRequest(const PageCacheRequest&);
  PageCacheRequest(PageCacheRequest&&);
  PageCacheRequest& operator=(const PageCacheRequest&);
  PageCacheRequest& operator=(PageCacheRequest&&);
  ~PageCacheRequest();

  // In UTF-8, a valid URL.
  std::string url;
  std::string method;
  std::vector<std::pair<std::string, std::string>> headers;
};

// What travels of a response: blink.mojom.FetchAPIResponse's fields that the
// Runtime has.
struct PageCacheResponse {
  PageCacheResponse();
  PageCacheResponse(const PageCacheResponse&);
  PageCacheResponse(PageCacheResponse&&);
  PageCacheResponse& operator=(const PageCacheResponse&);
  PageCacheResponse& operator=(PageCacheResponse&&);
  ~PageCacheResponse();

  // In UTF-8, each a valid URL.
  std::vector<std::string> url_list;
  uint16_t status = 200;
  std::string status_text;
  // network.mojom.FetchResponseType.
  uint8_t type = 2;
  // Names in lower case, each once.
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> cors_exposed_header_names;
  // base::Time in microseconds since the Windows epoch.
  int64_t response_time = 0;
  // The body: its size, and its number, which a put's sender gives and a
  // match's names for kReadBody, 0 for none.
  uint64_t body_size = 0;
  uint64_t body = 0;
};

// A Cache's put or delete.
struct PageCacheOperation {
  PageCacheOperation();
  PageCacheOperation(const PageCacheOperation&);
  PageCacheOperation(PageCacheOperation&&);
  PageCacheOperation& operator=(const PageCacheOperation&);
  PageCacheOperation& operator=(PageCacheOperation&&);
  ~PageCacheOperation();

  bool put = true;
  PageCacheRequest request;
  // A put's.
  PageCacheResponse response;
  // A delete's.
  PageCacheQueryOptions options;
};

struct PageCacheStorageCall {
  PageCacheStorageCall();
  PageCacheStorageCall(PageCacheStorageCall&&);
  PageCacheStorageCall& operator=(PageCacheStorageCall&&);
  ~PageCacheStorageCall();

  CacheStorageCallOp op = CacheStorageCallOp::kOpen;
  // The number of the question, which the answer names: larger than that of
  // each question before it.
  uint64_t question = 0;
  // kOpen, kHas, kDelete, and kMatch with a cache's name: in UTF-16, as a page
  // gave it.
  std::u16string cache_name;
  bool has_cache_name = false;
  // The kCache* operations' and kReleaseCache's cache.
  uint64_t cache = 0;
  std::optional<PageCacheRequest> request;
  PageCacheQueryOptions options;
  std::vector<PageCacheOperation> operations;
  // kBodyPart's body, kReadBody's, kBodyCredit's and kReleaseBody's.
  uint64_t body = 0;
  // kBodyPart's bytes.
  std::string bytes;
  // kBodyCredit's.
  uint32_t credit = 0;
};

struct PageCacheStorageAnswer {
  PageCacheStorageAnswer();
  PageCacheStorageAnswer(PageCacheStorageAnswer&&);
  PageCacheStorageAnswer& operator=(PageCacheStorageAnswer&&);
  ~PageCacheStorageAnswer();

  CacheStorageAnswerOp op = CacheStorageAnswerOp::kOpened;
  uint64_t question = 0;
  // blink.mojom.CacheStorageError.
  uint8_t error = 0;
  uint64_t cache = 0;
  bool value = false;
  std::vector<std::u16string> names;
  std::vector<PageCacheResponse> responses;
  std::vector<PageCacheRequest> requests;
  std::optional<std::string> message;
  // kBodyWanted's, kBodyData's and kBodyEnd's body.
  uint64_t body = 0;
  // kBodyWanted's.
  uint32_t most = 0;
  // kBodyData's.
  std::string bytes;
  // kBodyEnd's: 0 when all of the body came, and otherwise a net error.
  int32_t net_error = 0;
  // kEstimated's.
  int64_t usage = 0;
  int64_t quota = 0;
  int64_t indexed_db = 0;
  int64_t caches = 0;
};

// The payload of each, which fits in a frame if the *Fits() say so.
bool CacheStorageCallFits(const PageCacheStorageCall& call);
std::string EncodeCacheStorageCall(const PageCacheStorageCall& call);
bool CacheStorageAnswerFits(const PageCacheStorageAnswer& answer);
std::string EncodeCacheStorageAnswer(const PageCacheStorageAnswer& answer);

base::expected<PageCacheStorageCall, std::string> ReadCacheStorageCall(
    std::string_view payload);
base::expected<PageCacheStorageAnswer, std::string> ReadCacheStorageAnswer(
    std::string_view payload);

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_MESSAGES_H_
