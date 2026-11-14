#include "mitto/dom/toggle_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_toggle_event.h"
#include "mitto/dom/element.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// static
ToggleEvent* ToggleEvent::Create(const std::u16string& type,
                                 const bindings::ToggleEventInit& init) {
  return bindings::MakeGarbageCollected<ToggleEvent>(type, init);
}

// static
ToggleEvent* ToggleEvent::CreateTrusted(const std::u16string& type,
                                        const std::u16string& old_state,
                                        const std::u16string& new_state,
                                        bool cancelable) {
  bindings::ToggleEventInit init;
  init.cancelable = cancelable;
  init.old_state = old_state;
  init.new_state = new_state;
  ToggleEvent* const event = Create(type, init);
  event->SetTrusted(true);
  return event;
}

ToggleEvent::ToggleEvent(const std::u16string& type,
                         const bindings::ToggleEventInit& init)
    : Event(type, init),
      old_state_(init.old_state),
      new_state_(init.new_state),
      source_(init.source) {}

ToggleEvent::~ToggleEvent() = default;

const bindings::WrapperTypeInfo* ToggleEvent::GetWrapperTypeInfo() const {
  return &bindings::V8ToggleEvent::kWrapperTypeInfo;
}

void ToggleEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(source_);
}

}  // namespace mitto
