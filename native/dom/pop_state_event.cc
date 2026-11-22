#include "mitto/dom/pop_state_event.h"

#include <utility>

#include "mitto/bindings/v8_pop_state_event.h"
#include "mitto/dom/history.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
PopStateEvent* PopStateEvent::Create(const std::u16string& type,
                                     const bindings::PopStateEventInit& init) {
  return bindings::MakeGarbageCollected<PopStateEvent>(type, init);
}

// static
PopStateEvent* PopStateEvent::CreateForHistory(
    std::shared_ptr<const SerializedScriptValue> state,
    History* history) {
  PopStateEvent* const event =
      bindings::MakeGarbageCollected<PopStateEvent>(std::move(state), history);
  event->SetTrusted(true);
  return event;
}

PopStateEvent::PopStateEvent(const std::u16string& type,
                             const bindings::PopStateEventInit& init)
    : Event(type, init),
      state_(v8::Isolate::GetCurrent(), init.state),
      has_ua_visual_transition_(init.has_ua_visual_transition) {}

PopStateEvent::PopStateEvent(std::shared_ptr<const SerializedScriptValue> state,
                             History* history)
    : Event(u"popstate", bindings::EventInit()),
      serialized_state_(std::move(state)),
      history_(history),
      has_ua_visual_transition_(false) {}

PopStateEvent::~PopStateEvent() = default;

const bindings::WrapperTypeInfo* PopStateEvent::GetWrapperTypeInfo() const {
  return &bindings::V8PopStateEvent::kWrapperTypeInfo;
}

void PopStateEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(state_);
  visitor->Trace(history_);
}

v8::Local<v8::Value> PopStateEvent::State(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!state_.IsEmpty()) {
    return state_.Get(isolate);
  }
  v8::Local<v8::Value> state;
  if (history_ && history_->IsSameAsCurrentState(serialized_state_.get())) {
    state = history_->StateHelper(exception_state);
  } else if (serialized_state_) {
    state = serialized_state_->Deserialize(isolate);
  } else {
    state = v8::Null(isolate);
  }
  state_.Reset(isolate, state);
  return state;
}

}  // namespace mitto
