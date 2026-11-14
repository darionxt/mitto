#include "mitto/dom/before_unload_event.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_before_unload_event.h"

namespace mitto {

// static
BeforeUnloadEvent* BeforeUnloadEvent::Create() {
  BeforeUnloadEvent* const event =
      bindings::MakeGarbageCollected<BeforeUnloadEvent>();
  event->SetTrusted(true);
  return event;
}

// Document::DispatchBeforeUnloadEvent() calls initEvent() with no bubbling,
// and cancelable.
BeforeUnloadEvent::BeforeUnloadEvent()
    : Event(u"beforeunload", [] {
        bindings::EventInit init;
        init.cancelable = true;
        return init;
      }()) {}

BeforeUnloadEvent::~BeforeUnloadEvent() = default;

const bindings::WrapperTypeInfo* BeforeUnloadEvent::GetWrapperTypeInfo() const {
  return &bindings::V8BeforeUnloadEvent::kWrapperTypeInfo;
}

bool BeforeUnloadEvent::IsBeforeUnloadEvent() const {
  return true;
}

}  // namespace mitto
