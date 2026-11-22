// One fetch() of a page's: it settles fetch()'s promise with a Response once
// the response starts, or rejects it, and then brings the Response its body,
// whose bytes wait in a BodyLoaderBytesConsumer until they are read, as in a
// data pipe, and which Blink's BufferingBytesConsumer reads 50 ms after it
// starts unless the response has Cache-Control: no-store. The Fetcher owns it
// until the request ends. Its request's signal rejects the promise with its
// reason and cancels the request, as Blink's FetchManager::Loader::Abort()
// does. A body that streams goes through a BytesUploader.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_response_data.cc

#ifndef MITTO_DOM_FETCH_LOADER_H_
#define MITTO_DOM_FETCH_LOADER_H_

#include <stddef.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/body_loader_bytes_consumer.h"
#include "mitto/dom/fetcher.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class AbortSignal;
class BytesConsumer;
class BytesUploader;
class ReadableStream;

class FetchLoader final : public FetchClient, public BodyLoader {
 public:
  // `resolver` is fetch()'s, in `context`. `credentials` says that the
  // request's credentials mode is include. `signal` is the request's, which
  // has not aborted. `upload_body`, if any, is what the body that streams
  // reads, which nothing has read, from `upload_stream`.
  FetchLoader(v8::Isolate* isolate,
              v8::Local<v8::Context> context,
              v8::Local<v8::Promise::Resolver> resolver,
              bool credentials,
              AbortSignal* signal,
              BytesConsumer* upload_body = nullptr,
              ReadableStream* upload_stream = nullptr);

  FetchLoader(const FetchLoader&) = delete;
  FetchLoader& operator=(const FetchLoader&) = delete;

  ~FetchLoader() override;

  // FetchClient:
  void OnStarted(FetchController& controller) override;
  void OnResponseStarted(const FetchResponseHead& head) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseEnded(int net_error) override;
  void OnBodyRoom() override;

  // BodyLoader: it cancels the request once the body is dropped.
  void BodyRequested() override;
  void BodyTaken(size_t bytes) override;
  void BodyDropped() override;
  // The request's signal aborted. Destroys this.
  void Abort();

 private:
  void Cancel();

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Promise::Resolver> resolver_;
  const bool credentials_;
  raw_ptr<FetchController> controller_ = nullptr;
  // The request's signal, which the request keeps while it runs.
  cppgc::Persistent<AbortSignal> signal_;
  // Where the body goes, which goes with what reads it.
  cppgc::WeakPersistent<BodyLoaderBytesConsumer> consumer_;
  // The body that streams, until the request starts, and then its upload,
  // and the stream it reads, which an abort cancels.
  cppgc::Persistent<BytesConsumer> upload_body_;
  cppgc::Persistent<BytesUploader> uploader_;
  cppgc::Persistent<ReadableStream> upload_stream_;
  base::WeakPtrFactory<FetchLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_FETCH_LOADER_H_
