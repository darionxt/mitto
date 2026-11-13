// One fetch() of a page's: it settles fetch()'s promise with a Response once
// the response starts, or rejects it, and then brings the Response its body.
// The Fetcher owns it until the request ends. Its request's signal rejects
// the promise with its reason and cancels the request, as Blink's
// FetchManager::Loader::Abort() does.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_response_data.cc

#ifndef MITTO_DOM_FETCH_LOADER_H_
#define MITTO_DOM_FETCH_LOADER_H_

#include <stddef.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/fetcher.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class AbortSignal;
class Response;

class FetchLoader final : public FetchClient {
 public:
  // `resolver` is fetch()'s, in `context`. The response of a HEAD request,
  // `head`, has a null body. `signal` is the request's, which has not
  // aborted.
  FetchLoader(v8::Isolate* isolate,
              v8::Local<v8::Context> context,
              v8::Local<v8::Promise::Resolver> resolver,
              bool head,
              AbortSignal* signal);

  FetchLoader(const FetchLoader&) = delete;
  FetchLoader& operator=(const FetchLoader&) = delete;

  ~FetchLoader() override;

  // FetchClient:
  void OnStarted(FetchController& controller) override;
  void OnResponseStarted(const FetchResponseHead& head) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseEnded(int net_error) override;

  // What its Response tells it: a script reads the body, so what has come and
  // what comes is taken.
  void ReaderStarted();
  // The Response has been collected, so the rest of the body is not wanted.
  // Called during garbage collection, which may not run scripts or free
  // handles, so it cancels the request from a later task.
  void ResponseCollected();
  // The request's signal aborted. Destroys this.
  void Abort();

 private:
  void Cancel();

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Promise::Resolver> resolver_;
  const bool head_;
  raw_ptr<FetchController> controller_ = nullptr;
  // The request's signal, which the request keeps while it runs.
  cppgc::Persistent<AbortSignal> signal_;
  // The Response, which a script may let go until it reads the body.
  cppgc::WeakPersistent<Response> response_;
  // The Response while a script reads its body.
  cppgc::Persistent<Response> reading_;
  // Bytes the Response holds that the Session has no credit for yet.
  size_t untaken_ = 0;
  base::WeakPtrFactory<FetchLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_FETCH_LOADER_H_
