#include "mitto/dom/page_reveal_event.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_page_reveal_event.h"

namespace mitto {

// static
PageRevealEvent* PageRevealEvent::CreateTrusted() {
  PageRevealEvent* const event =
      bindings::MakeGarbageCollected<PageRevealEvent>();
  event->SetTrusted(true);
  return event;
}

// static
bindings::ScriptWrappable* PageRevealEvent::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<PageRevealEvent>(
      info, bindings::V8PageRevealEvent::kWrapperTypeInfo);
}

PageRevealEvent::PageRevealEvent()
    : Event(u"pagereveal", bindings::EventInit()) {}

PageRevealEvent::~PageRevealEvent() = default;

const bindings::WrapperTypeInfo* PageRevealEvent::GetWrapperTypeInfo() const {
  return &bindings::V8PageRevealEvent::kWrapperTypeInfo;
}

}  // namespace mitto
