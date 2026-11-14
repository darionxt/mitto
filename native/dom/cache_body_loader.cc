#include "mitto/dom/cache_body_loader.h"

#include <utility>

#include "base/functional/bind.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

CacheBodyLoader::CacheBodyLoader(CacheStorageHost& host,
                                 uint64_t body,
                                 Response* response,
                                 v8::Isolate* isolate,
                                 v8::Local<v8::Context> context)
    : host_(host),
      body_(body),
      isolate_(isolate),
      context_(isolate, context),
      response_(response) {}

CacheBodyLoader::~CacheBodyLoader() {
  if (Response* const response = response_.Get()) {
    response->loader_ = nullptr;
  }
}

void CacheBodyLoader::OnBodyData(std::string bytes) {
  Response* const response = response_.Get();
  if (!response) {
    // The Response went, and the body is about to be released.
    return;
  }
  untaken_ += bytes.size();
  response->AppendBody(bytes);
  if (response->HasReader()) {
    const size_t taken = untaken_;
    untaken_ = 0;
    host_->CreditStoredBody(body_, static_cast<uint32_t>(taken));
  }
}

void CacheBodyLoader::OnBodyEnd(int net_error) {
  if (Response* const response = response_.Get()) {
    response->loader_ = nullptr;
    // The end settles what reads the body, in the Response's context.
    RunInCacheContext(isolate_, context_, [&](v8::Local<v8::Context> context) {
      response->EndBody(net_error);
    });
  }
  reading_.Clear();
}

void CacheBodyLoader::ReaderStarted() {
  reading_ = response_.Get();
  if (!started_) {
    started_ = true;
    host_->ReadStoredBody(body_);
    return;
  }
  if (untaken_ > 0) {
    const size_t taken = untaken_;
    untaken_ = 0;
    host_->CreditStoredBody(body_, static_cast<uint32_t>(taken));
  }
}

void CacheBodyLoader::ResponseCollected() {
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
