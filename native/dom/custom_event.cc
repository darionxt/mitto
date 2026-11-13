#include "mitto/dom/custom_event.h"

#include "mitto/bindings/v8_custom_event.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
CustomEvent* CustomEvent::Create(const std::u16string& type,
                                 const bindings::CustomEventInit& init) {
  return bindings::MakeGarbageCollected<CustomEvent>(type, init);
}

CustomEvent::CustomEvent(const std::u16string& type,
                         const bindings::CustomEventInit& init)
    : Event(type, init) {
  if (!init.detail.IsEmpty() && !init.detail->IsNullOrUndefined()) {
    detail_.Reset(v8::Isolate::GetCurrent(), init.detail);
  }
}

CustomEvent::~CustomEvent() = default;

const bindings::WrapperTypeInfo* CustomEvent::GetWrapperTypeInfo() const {
  return &bindings::V8CustomEvent::kWrapperTypeInfo;
}

void CustomEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(detail_);
}

v8::Local<v8::Value> CustomEvent::Detail() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (detail_.IsEmpty()) {
    return v8::Null(isolate);
  }
  return detail_.Get(isolate);
}

void CustomEvent::InitCustomEvent(const std::u16string& type,
                                  bool bubbles,
                                  bool cancelable,
                                  v8::Local<v8::Value> detail) {
  InitEvent(type, bubbles, cancelable);
  if (!IsBeingDispatched() && !detail.IsEmpty()) {
    detail_.Reset(v8::Isolate::GetCurrent(), detail);
  }
}

}  // namespace mitto
