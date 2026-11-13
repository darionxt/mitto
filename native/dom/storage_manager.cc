#include "mitto/dom/storage_manager.h"

#include <stdint.h>

#include "mitto/bindings/v8_storage_manager.h"
#include "mitto/dom/script_values.h"
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
