// What the Cache API keeps and answers, as Chrome's browser has it: the
// request and response of each entry, which blink.mojom's FetchAPIRequest
// and FetchAPIResponse carry between a renderer and the browser, the options
// of a query, and the errors that a renderer turns into a page's exceptions.
//
// Upstream: third_party/blink/public/mojom/cache_storage/cache_storage.mojom
// Upstream: third_party/blink/public/mojom/fetch/fetch_api_request.mojom
// Upstream: third_party/blink/public/mojom/fetch/fetch_api_response.mojom
// Upstream: content/browser/cache_storage/cache_storage.proto

#ifndef MITTO_CACHE_STORAGE_CACHE_STORAGE_TYPES_H_
#define MITTO_CACHE_STORAGE_CACHE_STORAGE_TYPES_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/numerics/checked_math.h"
#include "base/time/time.h"
#include "url/gurl.h"

namespace mitto {

// blink.mojom.CacheStorageError, with its values.
enum class CacheStorageError : uint8_t {
  kSuccess = 0,
  kErrorExists = 1,
  kErrorStorage = 2,
  kErrorNotFound = 3,
  kErrorQuotaExceeded = 4,
  kErrorCacheNameNotFound = 5,
  kErrorQueryTooLarge = 6,
  kErrorNotImplemented = 7,
  kErrorDuplicateOperation = 8,
  kErrorCrossOriginResourcePolicy = 9,
  kMaxValue = kErrorCrossOriginResourcePolicy,
};

// network.mojom.FetchResponseType.
enum class CacheResponseType : uint8_t {
  kBasic = 0,
  kCors = 1,
  kDefault = 2,
  kError = 3,
  kOpaque = 4,
  kOpaqueRedirect = 5,
  kMaxValue = kOpaqueRedirect,
};

// Header lines by name, as the mojom's maps hold them: a request's names as
// the renderer wrote them, a response's in lower case, each once.
using CacheHeaderMap = base::flat_map<std::string, std::string>;

// The request of an entry: what CacheMetadata's CacheRequest keeps of a
// FetchAPIRequest, beside the URL.
struct CacheRequest {
  CacheRequest();
  CacheRequest(const CacheRequest&);
  CacheRequest(CacheRequest&&);
  CacheRequest& operator=(const CacheRequest&);
  CacheRequest& operator=(CacheRequest&&);
  ~CacheRequest();

  // With its fragment, which the entry keeps apart from its key.
  GURL url;
  // Empty in a query that matches any method.
  std::string method;
  CacheHeaderMap headers;
  bool is_reload = false;
  bool is_history_navigation = false;
};

// The response of an entry: what CacheMetadata's CacheResponse keeps of a
// FetchAPIResponse, and the size of its body.
struct CacheResponse {
  CacheResponse();
  CacheResponse(const CacheResponse&);
  CacheResponse(CacheResponse&&);
  CacheResponse& operator=(const CacheResponse&);
  CacheResponse& operator=(CacheResponse&&);
  ~CacheResponse();

  std::vector<GURL> url_list;
  int32_t status_code = 200;
  std::string status_text;
  CacheResponseType response_type = CacheResponseType::kDefault;
  CacheHeaderMap headers;
  std::vector<std::string> cors_exposed_header_names;
  base::Time response_time;
  std::optional<std::string> mime_type;
  std::optional<std::string> request_method;
  // What an opaque response adds to the cache's size, which the renderer
  // works out, and what its side data adds.
  int64_t padding = 0;
  int64_t side_data_padding = 0;
  // net::HttpConnectionInfo, as a number.
  int32_t connection_info = 0;
  std::string alpn_negotiated_protocol = "unknown";
  bool was_fetched_via_spdy = false;
  bool request_include_credentials = true;
  bool timing_allow_passed = true;
  bool has_range_requested = false;
  // The size of its body: 0 for none.
  uint64_t body_size = 0;
};

// blink.mojom.CacheQueryOptions.
struct CacheQueryOptions {
  bool ignore_search = false;
  bool ignore_method = false;
  bool ignore_vary = false;
};

// What disk_cache keeps in stream 0 of an entry: CacheMetadata, written with
// base::Pickle rather than as the protocol buffer Chrome writes, since no
// page sees it.
struct CacheMetadata {
  CacheMetadata();
  CacheMetadata(CacheMetadata&&);
  CacheMetadata& operator=(CacheMetadata&&);
  ~CacheMetadata();

  // The request without its URL, which is the entry's key, and the fragment
  // that the key lacks.
  CacheRequest request;
  std::optional<std::string> fragment;
  CacheResponse response;
  base::Time entry_time;
};

// The streams of an entry: CacheStorageCache's EntryIndex.
inline constexpr int kCacheEntryHeaders = 0;
inline constexpr int kCacheEntryResponseBody = 1;
inline constexpr int kCacheEntrySideData = 2;

std::string SerializeCacheMetadata(const CacheMetadata& metadata);
std::optional<CacheMetadata> ParseCacheMetadata(const std::string& bytes);

// NormalizeCacheUrl(): the URL without its fragment, which an entry's key is.
GURL NormalizeCacheUrl(const GURL& url);

// VaryMatches(): whether `request` asks for what `cached_request` stored,
// given the response's Vary line.
bool VaryMatches(const CacheHeaderMap& request,
                 const CacheHeaderMap& cached_request,
                 CacheResponseType response_type,
                 const CacheHeaderMap& response_headers);

// Whether a response of `type` counts with padding: opaque ones, whose size
// would tell another origin's secrets.
bool ShouldPadResponseType(CacheResponseType type);

// CalculateRequiredSafeSpaceForRequest() and ...Response(): what a put of
// them needs of the quota, before it writes anything.
base::CheckedNumeric<uint64_t> RequiredSpaceForRequest(
    const CacheRequest& request);
base::CheckedNumeric<uint64_t> RequiredSpaceForResponse(
    const CacheResponse& response);

// EstimatedStructSize() and EstimatedResponseSizeWithoutBlob(): what a query
// result counts toward its cap.
size_t EstimatedRequestSize(const CacheRequest& request);
// `cache_name_size` is the size of the name of the response's cache in UTF-8.
size_t EstimatedResponseSize(const CacheResponse& response,
                             size_t cache_name_size);

}  // namespace mitto

#endif  // MITTO_CACHE_STORAGE_CACHE_STORAGE_TYPES_H_
