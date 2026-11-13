#include "mitto/dom/lock.h"

#include <tuple>
#include <utility>

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_lock.h"
#include "mitto/dom/lock_manager.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

Lock::Lock(std::u16string name,
           LockService::Mode mode,
           int64_t lock_id,
           LockManager* manager)
    : name_(std::move(name)),
      mode_(mode),
      lock_id_(lock_id),
      manager_(manager) {}

Lock::~Lock() = default;

const bindings::WrapperTypeInfo* Lock::GetWrapperTypeInfo() const {
  return &bindings::V8Lock::kWrapperTypeInfo;
}

void Lock::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(manager_);
  visitor->Trace(resolver_);
}

// static
std::u16string Lock::ModeName(LockService::Mode mode) {
  return mode == LockService::Mode::kShared ? u"shared" : u"exclusive";
}

std::u16string Lock::Mode() const {
  return ModeName(mode_);
}

void Lock::HoldUntil(v8::Local<v8::Context> context,
                     v8::Local<v8::Promise> promise,
                     v8::Local<v8::Promise::Resolver> resolver) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  resolver_.Reset(isolate, resolver);
  // Lock::ThenFunction: each reaction holds the lock's wrapper, and with it
  // the lock, until the promise settles.
  const v8::Local<v8::Value> self = bindings::ToV8(isolate, this);
  v8::Local<v8::Function> on_fulfilled;
  v8::Local<v8::Function> on_rejected;
  if (!v8::Function::New(
           context,
           [](const v8::FunctionCallbackInfo<v8::Value>& info) {
             Settled(info, /*fulfilled=*/true);
           },
           self, 1)
           .ToLocal(&on_fulfilled) ||
      !v8::Function::New(
           context,
           [](const v8::FunctionCallbackInfo<v8::Value>& info) {
             Settled(info, /*fulfilled=*/false);
           },
           self, 1)
           .ToLocal(&on_rejected)) {
    return;
  }
  std::ignore = promise->Then(context, on_fulfilled, on_rejected);
}

// static
void Lock::Settled(const v8::FunctionCallbackInfo<v8::Value>& info,
                   bool fulfilled) {
  v8::Isolate* const isolate = info.GetIsolate();
  Lock* const lock = bindings::ToWrappableOf<Lock>(
      isolate, bindings::V8Lock::kWrapperTypeInfo, info.Data());
  if (!lock || lock->resolver_.IsEmpty()) {
    return;
  }
  lock->ReleaseIfHeld();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Promise::Resolver> resolver =
      lock->resolver_.Get(isolate);
  if (fulfilled) {
    std::ignore = resolver->Resolve(context, info[0]);
  } else {
    std::ignore = resolver->Reject(context, info[0]);
  }
  lock->resolver_.Reset();
}

void Lock::ReleaseIfHeld() {
  if (!held_) {
    return;
  }
  // Dropping the handle lets the lock go in the browser.
  held_ = false;
  manager_->OnLockReleased(this);
}

// Lock::OnConnectionError().
void Lock::OnBroken(v8::Local<v8::Context> context) {
  ReleaseIfHeld();
  if (resolver_.IsEmpty()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Value> error =
      bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
          isolate, "AbortError",
          "Lock broken by another request with the 'steal' option.");
  std::ignore = resolver_.Get(isolate)->Reject(context, error);
}

}  // namespace mitto
