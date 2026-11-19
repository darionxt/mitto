#include "mitto/dom/storage_bucket.h"

#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_storage_bucket.h"
#include "mitto/dom/cache_storage.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/idb_factory.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

// Blink's DOMException of InvalidStateError with no message of its own.
constexpr char16_t kInvalidStateMessage[] =
    u"An attempt was made to use an object that is not, or is no longer, "
    u"usable.";

v8::Local<v8::Value> UnknownError(v8::Isolate* isolate,
                                  const char16_t* message) {
  return DOMException::Create(message, u"UnknownError")->ToV8(isolate);
}

void RejectUnknown(v8::Isolate* isolate,
                   v8::Local<v8::Context> context,
                   v8::Local<v8::Promise::Resolver> resolver,
                   const char16_t* message) {
  std::ignore = resolver->Reject(context, UnknownError(isolate, message));
}

}  // namespace

StorageBucket::StorageBucket(Window* window,
                             StorageBucketHost* host,
                             std::u16string name,
                             uint64_t bucket)
    : window_(window), host_(host), name_(std::move(name)), bucket_(bucket) {}

StorageBucket::~StorageBucket() = default;

const bindings::WrapperTypeInfo* StorageBucket::GetWrapperTypeInfo() const {
  return &bindings::V8StorageBucket::kWrapperTypeInfo;
}

void StorageBucket::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(indexed_db_);
  visitor->Trace(caches_);
}

void StorageBucket::Dispose() {
  // Garbage collection may not send, so the bucket is released from a task.
  EventLoop::Get()
      ->TaskRunner(TaskType::kMiscPlatformAPI)
      ->PostTask(FROM_HERE,
                 base::BindOnce([](StorageBucketHost* host,
                                   uint64_t bucket) { host->Release(bucket); },
                                host_.get(), bucket_));
}

StorageBucketHost* StorageBucket::Host() const {
  Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    return nullptr;
  }
  return host_;
}

v8::Local<v8::Promise> StorageBucket::Ask(PageStorageBucketCall call,
                                          Settle settle) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  StorageBucketHost* const host = Host();
  if (!host) {
    // The remote is unbound once the context has gone.
    std::ignore = resolver->Reject(
        context,
        DOMException::Create(kInvalidStateMessage, u"InvalidStateError")
            ->ToV8(isolate));
    return resolver->GetPromise();
  }
  call.bucket = bucket_;
  host->Ask(std::move(call),
            base::BindOnce(
                [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
                   v8::Global<v8::Promise::Resolver> resolver_handle,
                   Settle settle, PageStorageBucketAnswer answer) {
                  RunInCacheContext(isolate, context_handle,
                                    [&](v8::Local<v8::Context> context) {
                                      settle(isolate, context,
                                             resolver_handle.Get(isolate),
                                             answer);
                                    });
                },
                isolate, v8::Global<v8::Context>(isolate, context),
                v8::Global<v8::Promise::Resolver>(isolate, resolver), settle));
  return resolver->GetPromise();
}

v8::Local<v8::Promise> StorageBucket::Persist() {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kPersist;
  return Ask(
      std::move(call), [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer) {
        if (!answer.success) {
          RejectUnknown(isolate, context, resolver,
                        u"Unknown error occurred while requesting persist.");
          return;
        }
        std::ignore =
            resolver->Resolve(context, v8::Boolean::New(isolate, answer.value));
      });
}

v8::Local<v8::Promise> StorageBucket::Persisted() {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kPersisted;
  return Ask(
      std::move(call), [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer) {
        if (!answer.success) {
          RejectUnknown(isolate, context, resolver,
                        u"Unknown error occurred while getting persisted.");
          return;
        }
        std::ignore =
            resolver->Resolve(context, v8::Boolean::New(isolate, answer.value));
      });
}

v8::Local<v8::Promise> StorageBucket::Estimate() {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kEstimate;
  return Ask(std::move(call), [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 const PageStorageBucketAnswer& answer) {
    if (!answer.success) {
      RejectUnknown(isolate, context, resolver,
                    u"Unknown error occurred while getting estimate.");
      return;
    }
    // DidGetEstimate(): a StorageEstimate whose usageDetails is empty.
    std::ignore = resolver->Resolve(
        context,
        PlainObject(isolate,
                    {{"quota", v8::Number::New(
                                   isolate, static_cast<double>(answer.quota))},
                     {"usage", v8::Number::New(
                                   isolate, static_cast<double>(answer.usage))},
                     {"usageDetails", v8::Object::New(isolate)}}));
  });
}

v8::Local<v8::Promise> StorageBucket::Durability() {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kDurability;
  return Ask(
      std::move(call), [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer) {
        if (!answer.success) {
          RejectUnknown(isolate, context, resolver,
                        u"Unknown error occurred while getting durability.");
          return;
        }
        std::ignore = resolver->Resolve(
            context, answer.durability == 1
                         ? v8::String::NewFromUtf8Literal(isolate, "strict")
                         : v8::String::NewFromUtf8Literal(isolate, "relaxed"));
      });
}

v8::Local<v8::Promise> StorageBucket::SetExpires(double expires) {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kSetExpires;
  call.expires = base::Time::FromMillisecondsSinceUnixEpoch(expires)
                     .ToDeltaSinceWindowsEpoch()
                     .InMicroseconds();
  return Ask(
      std::move(call), [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer) {
        if (!answer.success) {
          RejectUnknown(isolate, context, resolver,
                        u"Unknown error occurred while setting expires.");
          return;
        }
        std::ignore = resolver->Resolve(context, v8::Undefined(isolate));
      });
}

v8::Local<v8::Promise> StorageBucket::Expires() {
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kExpires;
  return Ask(
      std::move(call), [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer) {
        if (!answer.success) {
          RejectUnknown(isolate, context, resolver,
                        u"Unknown error occurred while getting expires.");
          return;
        }
        if (!answer.expires) {
          std::ignore = resolver->Resolve(context, v8::Null(isolate));
          return;
        }
        // ToV8Traits<IDLNullable<IDLDOMHighResTimeStamp>> of a base::Time: its
        // microseconds over 1000, and an infinity for the largest or smallest.
        const base::Time expires = base::Time::FromDeltaSinceWindowsEpoch(
            base::Microseconds(*answer.expires));
        std::ignore = resolver->Resolve(
            context,
            v8::Number::New(
                isolate, expires.InMillisecondsFSinceUnixEpochIgnoringNull()));
      });
}

IDBFactory* StorageBucket::IndexedDB() {
  if (!indexed_db_) {
    indexed_db_ =
        bindings::MakeGarbageCollected<IDBFactory>(window_.Get(), this);
  }
  return indexed_db_.Get();
}

CacheStorage* StorageBucket::Caches(bindings::ExceptionState& exception_state) {
  // GlobalCacheStorage::CanCreateCacheStorage(), as the window's caches.
  Window* const window = window_.Get();
  if (!window || !window->HasContext() ||
      !window->GetDocument().GetCacheStorageHost()) {
    exception_state.ThrowDOMException("SecurityError",
                                      "Access to cache storage is denied.");
    return nullptr;
  }
  if (!caches_) {
    caches_ = bindings::MakeGarbageCollected<CacheStorage>(window, this);
  }
  return caches_.Get();
}

}  // namespace mitto
