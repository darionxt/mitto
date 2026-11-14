// What a document's CacheStorage and Caches share: the request that the
// Session gets of a page's Request, the page's objects of what the Session
// answers, and the exceptions of its errors, as Blink's cache_storage module
// has them.
//
// Upstream: third_party/blink/renderer/modules/cache_storage/cache_storage_error.cc
// Upstream: third_party/blink/renderer/core/fetch/request.cc
// Upstream: third_party/blink/renderer/core/fetch/response.cc

#ifndef MITTO_DOM_CACHE_STORAGE_ANSWERS_H_
#define MITTO_DOM_CACHE_STORAGE_ANSWERS_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/functional/function_ref.h"
#include "mitto/cache_storage_messages.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class CacheStorageHost;
class Request;
class Response;

// blink.mojom.CacheStorageError's values that the page sees.
inline constexpr uint8_t kCacheStorageSuccess = 0;
inline constexpr uint8_t kCacheStorageErrorStorage = 2;
inline constexpr uint8_t kCacheStorageErrorNotFound = 3;
inline constexpr uint8_t kCacheStorageErrorQuotaExceeded = 4;
inline constexpr uint8_t kCacheStorageErrorCacheNameNotFound = 5;
inline constexpr uint8_t kCacheStorageErrorQueryTooLarge = 6;

// RejectCacheStorageWithError(): the exception of `error`, with `message`, or
// with the error's own message when that is empty, in the context of the
// call `method` of `interface_name`, as Blink's resolver puts what it rejects
// with.
v8::Local<v8::Value> CacheStorageException(v8::Isolate* isolate,
                                           uint8_t error,
                                           const std::string& message,
                                           std::string_view method,
                                           std::string_view interface_name);

// What a resolver of the call `method` of `interface_name` rejects with that
// it makes itself: a DOMException of `name`, or a TypeError for an empty
// name, with `message` in the call's context.
v8::Local<v8::Value> CacheCallException(v8::Isolate* isolate,
                                        std::string_view name,
                                        std::string_view message,
                                        std::string_view method,
                                        std::string_view interface_name);

// Request::CreateFetchAPIRequest(): its URL, method and header lines, each
// name once with its values joined, without Referer.
PageCacheRequest ToPageCacheRequest(const Request& request);

// Request::Create() of a FetchAPIRequest that the Session answered.
Request* RequestFromCache(const PageCacheRequest& request);

// Response::Create() of a FetchAPIResponse that the Session answered, its
// header lines filtered as its type has them, whose body, if it has one,
// comes from `host` once a script reads it.
Response* ResponseFromCache(CacheStorageHost& host,
                            const PageCacheResponse& response);

// Whether the window of `context` is still there, which it is until the Page
// loads another document.
bool CacheContextIsAlive(v8::Isolate* isolate, v8::Local<v8::Context> context);

// Runs `run` with `context` entered and its microtasks run after, as a task
// of the Page settles a promise of it, unless its window has gone.
void RunInCacheContext(v8::Isolate* isolate,
                       const v8::Global<v8::Context>& context,
                       base::FunctionRef<void(v8::Local<v8::Context>)> run);

}  // namespace mitto

#endif  // MITTO_DOM_CACHE_STORAGE_ANSWERS_H_
