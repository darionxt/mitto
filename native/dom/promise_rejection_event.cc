#include "mitto/dom/promise_rejection_event.h"

#include "mitto/bindings/v8_promise_rejection_event.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
PromiseRejectionEvent* PromiseRejectionEvent::Create(
    const std::u16string& type,
    const bindings::PromiseRejectionEventInit& init) {
  return bindings::MakeGarbageCollected<PromiseRejectionEvent>(type, init);
}

// static
PromiseRejectionEvent* PromiseRejectionEvent::CreateForRejection(
    const std::u16string& type,
    bool cancelable,
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Value> reason) {
  bindings::PromiseRejectionEventInit init;
  init.cancelable = cancelable;
  init.promise = promise;
  init.reason = reason;
  return Create(type, init);
}

PromiseRejectionEvent::PromiseRejectionEvent(
    const std::u16string& type,
    const bindings::PromiseRejectionEventInit& init)
    : Event(type, init) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (init.promise && !init.promise->IsEmpty()) {
    promise_.Reset(isolate, *init.promise);
  }
  if (init.reason && !init.reason->IsEmpty()) {
    reason_.Reset(isolate, *init.reason);
  }
}

PromiseRejectionEvent::~PromiseRejectionEvent() = default;

const bindings::WrapperTypeInfo* PromiseRejectionEvent::GetWrapperTypeInfo()
    const {
  return &bindings::V8PromiseRejectionEvent::kWrapperTypeInfo;
}

void PromiseRejectionEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(promise_);
  visitor->Trace(reason_);
}

v8::Local<v8::Object> PromiseRejectionEvent::Promise() const {
  return promise_.Get(v8::Isolate::GetCurrent());
}

v8::Local<v8::Value> PromiseRejectionEvent::Reason() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (reason_.IsEmpty()) {
    return v8::Undefined(isolate);
  }
  return reason_.Get(isolate);
}

}  // namespace mitto
