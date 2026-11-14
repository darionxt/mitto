#include "mitto/dom/storage_manager.h"

#include <stdint.h>

#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "mitto/bindings/v8_storage_manager.h"
#include "mitto/cache_storage_messages.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/document.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// kStaticStorageQuota, on by default: a storage key's quota is its usage and
// 10 GiB on a disk of at least that, which a Machine always has, so that the
// quota does not tell the disk's size.
constexpr double kStaticQuotaBytes = 10.0 * 1024 * 1024 * 1024;

v8::Local<v8::Promise> ResolvedFromTask(v8::Isolate* isolate,
                                        v8::Local<v8::Value> value) {
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // Blink resolves each once the browser's quota manager has answered.
  ResolveFromTask(isolate, resolver, value);
  return resolver->GetPromise();
}

}  // namespace

// static
StorageManager* StorageManager::Create() {
  return bindings::MakeGarbageCollected<StorageManager>();
}

StorageManager::StorageManager() = default;
StorageManager::~StorageManager() = default;

const bindings::WrapperTypeInfo* StorageManager::GetWrapperTypeInfo() const {
  return &bindings::V8StorageManager::kWrapperTypeInfo;
}

v8::Local<v8::Promise> StorageManager::Estimate(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // A document that its Session loaded asks the Session's quota, as Blink asks
  // the browser's QuotaManagerHost, and its answer comes from a task of its
  // own.
  if (CacheStorageHost* const host =
          Window::Current().GetDocument().GetCacheStorageHost()) {
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
      return v8::Local<v8::Promise>();
    }
    PageCacheStorageCall call;
    call.op = CacheStorageCallOp::kEstimate;
    host->Ask(
        std::move(call),
        base::BindOnce(
            [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
               v8::Global<v8::Promise::Resolver> resolver_handle,
               PageCacheStorageAnswer answer) {
              RunInCacheContext(
                  isolate, context_handle, [&](v8::Local<v8::Context> context) {
                    // StorageUsageDetails lists only what is not 0, in the
                    // order of its members' names.
                    std::vector<std::pair<std::string_view,
                                          v8::Local<v8::Value>>>
                        details;
                    if (answer.caches) {
                      details.emplace_back(
                          "caches",
                          v8::Number::New(isolate,
                                          static_cast<double>(answer.caches)));
                    }
                    if (answer.indexed_db) {
                      details.emplace_back(
                          "indexedDB",
                          v8::Number::New(
                              isolate, static_cast<double>(answer.indexed_db)));
                    }
                    std::ignore = resolver_handle.Get(isolate)->Resolve(
                        context,
                        PlainObject(
                            isolate,
                            {{"quota",
                              v8::Number::New(
                                  isolate, static_cast<double>(answer.quota))},
                             {"usage",
                              v8::Number::New(
                                  isolate, static_cast<double>(answer.usage))},
                             {"usageDetails", PlainObject(isolate, details)}}));
                  });
            },
            isolate, v8::Global<v8::Context>(isolate, context),
            v8::Global<v8::Promise::Resolver>(isolate, resolver)));
    return resolver->GetPromise();
  }
  // A StorageEstimate, whose usageDetails lists only what is not 0.
  return ResolvedFromTask(
      isolate,
      PlainObject(isolate,
                  {{"quota", v8::Number::New(isolate, kStaticQuotaBytes)},
                   {"usage", v8::Number::New(isolate, 0)},
                   {"usageDetails", v8::Object::New(isolate)}}));
}

v8::Local<v8::Promise> StorageManager::Persisted(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  return ResolvedFromTask(isolate, v8::False(isolate));
}

v8::Local<v8::Promise> StorageManager::Persist(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  return ResolvedFromTask(isolate, v8::False(isolate));
}

}  // namespace mitto
