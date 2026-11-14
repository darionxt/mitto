// A Cache that CacheStorage.open() gave, as Blink's asks Chrome's browser for
// one cache of the document's storage key. put() checks the request and the
// response as Blink does, takes the response's body, and sends the entry in
// one batch; add() and addAll() fetch each request through the window's
// fetch() first, and fail as a whole if one fails. Each promise settles from a
// task of its own once the Session has answered. The Session keeps the cache
// open while this object lives.
//
// Upstream: third_party/blink/renderer/modules/cache_storage/cache.cc

#ifndef MITTO_DOM_CACHE_H_
#define MITTO_DOM_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/cache_storage_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class AbortController;
class CacheStorageHost;
class Request;
class Response;
class Window;

class Cache final : public bindings::ScriptWrappable {
  CPPGC_USING_PRE_FINALIZER(Cache, Dispose);

 public:
  // Cache `cache` of `host`'s, which `window`'s CacheStorage opened.
  Cache(Window* window, CacheStorageHost* host, uint64_t cache);
  ~Cache() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Match(const bindings::RequestOrUSVString& request,
                               const bindings::CacheQueryOptions& options,
                               bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> MatchAll(
      const std::optional<bindings::RequestOrUSVString>& request,
      const bindings::CacheQueryOptions& options,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Add(const bindings::RequestOrUSVString& request,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> AddAll(
      const std::vector<bindings::RequestOrUSVString>& requests,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Put(const bindings::RequestOrUSVString& request,
                             Response* response,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Delete(const bindings::RequestOrUSVString& request,
                                const bindings::CacheQueryOptions& options,
                                bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Keys(
      const std::optional<bindings::RequestOrUSVString>& request,
      const bindings::CacheQueryOptions& options,
      bindings::ExceptionState& exception_state);

 private:
  // BarrierCallbackForPutResponse: what a put, an add() or an addAll() waits
  // for, the responses and their bodies, by its number.
  struct PutBarrier {
    PutBarrier();
    PutBarrier(PutBarrier&&);
    PutBarrier& operator=(PutBarrier&&);
    ~PutBarrier();

    v8::Global<v8::Context> context;
    v8::Global<v8::Promise::Resolver> resolver;
    // Such as "Cache.put()", and the member it is, such as "put".
    std::string method_name;
    std::string member;
    std::vector<cppgc::Persistent<Request>> requests;
    std::vector<cppgc::Persistent<Response>> responses;
    std::vector<std::optional<std::string>> bodies;
    size_t complete = 0;
    bool stopped = false;
    // For more than one request, which aborts the fetches of the others when
    // one fails.
    cppgc::Persistent<AbortController> abort_controller;
  };

  void Dispose();
  // The host, or null once the window's context has gone.
  CacheStorageHost* Host() const;
  v8::Local<v8::Promise> AddAllImpl(std::string method_name,
                                    std::vector<Request*> requests,
                                    bindings::ExceptionState& exception_state);
  // Starts a barrier of `requests`, whose promise is `resolver`'s, and
  // returns its number.
  uint64_t StartBarrier(v8::Local<v8::Context> context,
                        v8::Local<v8::Promise::Resolver> resolver,
                        std::string method_name,
                        std::vector<Request*> requests);
  PutBarrier* FindBarrier(uint64_t barrier);
  // ResponseBodyLoader::OnResponse(): checks response `index` and takes its
  // body.
  void OnResponse(uint64_t barrier,
                  size_t index,
                  Response* response,
                  bool require_ok);
  void CompletedResponse(uint64_t barrier,
                         size_t index,
                         std::optional<std::string> body);
  // Rejects the barrier's promise with `exception` and stops it.
  void FailBarrier(uint64_t barrier, v8::Local<v8::Value> exception);
  // PutImpl() and BarrierCallbackForPutComplete: one batch of the entries.
  void PutAll(uint64_t barrier);
  // What the fetch of add() and addAll() calls back with.
  static void FetchSettled(const v8::FunctionCallbackInfo<v8::Value>& info,
                           bool fulfilled);
  // Asks the Session `call`, and settles the promise with what `settle` makes
  // of the answer, in the call's context.
  using Settle = void (*)(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          CacheStorageHost& host,
                          const PageCacheStorageAnswer& answer);
  // `method` is the call's, whose name a rejection that this makes says.
  v8::Local<v8::Promise> Ask(PageCacheStorageCall call,
                             std::string_view method,
                             Settle settle);

  const cppgc::WeakMember<Window> window_;
  // The Page's, which outlives every window.
  const raw_ptr<CacheStorageHost> host_;
  const uint64_t cache_;
  std::map<uint64_t, PutBarrier> barriers_;
  uint64_t next_barrier_ = 1;
};

}  // namespace mitto

#endif  // MITTO_DOM_CACHE_H_
