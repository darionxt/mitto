#include "mitto/dom/page_transition_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_page_transition_event.h"

namespace mitto {

// static
PageTransitionEvent* PageTransitionEvent::Create(
    const std::u16string& type,
    const bindings::PageTransitionEventInit& init) {
  return bindings::MakeGarbageCollected<PageTransitionEvent>(type, init);
}

// static
PageTransitionEvent* PageTransitionEvent::CreateTrusted(
    const std::u16string& type,
    bool persisted) {
  bindings::PageTransitionEventInit init;
  init.persisted = persisted;
  PageTransitionEvent* const event = Create(type, init);
  event->SetTrusted(true);
  return event;
}

PageTransitionEvent::PageTransitionEvent(
    const std::u16string& type,
    const bindings::PageTransitionEventInit& init)
    : Event(type, init), persisted_(init.persisted.value_or(false)) {}

PageTransitionEvent::~PageTransitionEvent() = default;

const bindings::WrapperTypeInfo* PageTransitionEvent::GetWrapperTypeInfo()
    const {
  return &bindings::V8PageTransitionEvent::kWrapperTypeInfo;
}

}  // namespace mitto
