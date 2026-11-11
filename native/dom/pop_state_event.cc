#include "mitto/dom/pop_state_event.h"

#include "mitto/bindings/v8_pop_state_event.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
PopStateEvent* PopStateEvent::Create(const std::u16string& type,
                                     const bindings::PopStateEventInit& init) {
  return bindings::MakeGarbageCollected<PopStateEvent>(type, init);
}

PopStateEvent::PopStateEvent(const std::u16string& type,
                             const bindings::PopStateEventInit& init)
    : Event(type, init),
      state_(v8::Isolate::GetCurrent(), init.state),
      has_ua_visual_transition_(init.has_ua_visual_transition) {}

const bindings::WrapperTypeInfo* PopStateEvent::GetWrapperTypeInfo() const {
  return &bindings::V8PopStateEvent::kWrapperTypeInfo;
}

void PopStateEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(state_);
}

v8::Local<v8::Value> PopStateEvent::State(
    bindings::ExceptionState& exception_state) const {
  // The state of an event that history fires is serialized, and reading it can
  // throw. A page's own event holds the value it was given.
  return state_.Get(v8::Isolate::GetCurrent());
}

}  // namespace mitto
