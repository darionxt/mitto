#include "mitto/dom/navigation_current_entry_change_event.h"

#include <utility>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_navigation_current_entry_change_event.h"
#include "mitto/dom/navigation_history_entry.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// static
NavigationCurrentEntryChangeEvent*
NavigationCurrentEntryChangeEvent::CreateTrusted(std::u16string navigation_type,
                                                 NavigationHistoryEntry* from) {
  NavigationCurrentEntryChangeEvent* const event =
      bindings::MakeGarbageCollected<NavigationCurrentEntryChangeEvent>(
          u"currententrychange", std::move(navigation_type), from);
  event->SetTrusted(true);
  return event;
}

// static
bindings::ScriptWrappable* NavigationCurrentEntryChangeEvent::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<NavigationCurrentEntryChangeEvent>(
      info, bindings::V8NavigationCurrentEntryChangeEvent::kWrapperTypeInfo,
      std::u16string(), std::nullopt, nullptr);
}

NavigationCurrentEntryChangeEvent::NavigationCurrentEntryChangeEvent(
    const std::u16string& type,
    std::optional<std::u16string> navigation_type,
    NavigationHistoryEntry* from)
    : Event(type, bindings::EventInit()),
      navigation_type_(std::move(navigation_type)),
      from_(from) {}

NavigationCurrentEntryChangeEvent::~NavigationCurrentEntryChangeEvent() =
    default;

const bindings::WrapperTypeInfo*
NavigationCurrentEntryChangeEvent::GetWrapperTypeInfo() const {
  return &bindings::V8NavigationCurrentEntryChangeEvent::kWrapperTypeInfo;
}

void NavigationCurrentEntryChangeEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(from_);
}

}  // namespace mitto
