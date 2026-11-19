#include "mitto/dom/cache_body_loader.h"

#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/event_loop.h"
#include "v8/include/v8-context.h"

namespace mitto {

CacheBodyLoader::CacheBodyLoader(CacheStorageHost& host,
                                 uint64_t body,
                                 v8::Isolate* isolate,
                                 v8::Local<v8::Context> context)
    : host_(host), body_(body), isolate_(isolate), context_(isolate, context) {
  consumer_ = bindings::MakeGarbageCollected<BodyLoaderBytesConsumer>(
      this, bindings::V8Window::ToWrappable(isolate, context->Global()));
}

CacheBodyLoader::~CacheBodyLoader() {
  if (BodyLoaderBytesConsumer* const consumer = consumer_.Get()) {
    consumer->DetachLoader();
  }
}

void CacheBodyLoader::OnBodyData(std::string bytes) {
  BodyLoaderBytesConsumer* const consumer = consumer_.Get();
  if (!consumer) {
    // What read the body went, and the body is about to be released.
    return;
  }
  RunInCacheContext(isolate_, context_, [&](v8::Local<v8::Context> context) {
    consumer->AppendBody(bytes);
  });
}

void CacheBodyLoader::OnBodyEnd(int net_error) {
  BodyLoaderBytesConsumer* const consumer = consumer_.Get();
  if (!consumer) {
    return;
  }
  consumer->DetachLoader();
  consumer_.Clear();
  // The end settles what reads the body, in its context.
  RunInCacheContext(isolate_, context_, [&](v8::Local<v8::Context> context) {
    if (net_error == 0) {
      consumer->SignalComplete();
    } else {
      consumer->SignalError();
    }
  });
}

void CacheBodyLoader::BodyRequested() {
  if (!started_) {
    started_ = true;
    host_->ReadStoredBody(body_);
  }
}

void CacheBodyLoader::BodyTaken(size_t bytes) {
  host_->CreditStoredBody(body_, static_cast<uint32_t>(bytes));
}

void CacheBodyLoader::BodyDropped() {
  EventLoop::Get()
      ->TaskRunner(TaskType::kMiscPlatformAPI)
      ->PostTask(FROM_HERE, base::BindOnce(&CacheBodyLoader::Release,
                                           weak_factory_.GetWeakPtr()));
}

void CacheBodyLoader::Release() {
  // Destroys this.
  host_->ReleaseStoredBody(body_);
}

}  // namespace mitto
