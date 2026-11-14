#include "mitto/dom/navigation_activation.h"

#include <utility>

#include "mitto/bindings/v8_navigation_activation.h"
#include "mitto/dom/navigation_history_entry.h"

namespace mitto {

// static
NavigationActivation* NavigationActivation::Create(
    NavigationHistoryEntry* entry,
    std::u16string navigation_type,
    NavigationHistoryEntry* from) {
  return bindings::MakeGarbageCollected<NavigationActivation>(
      entry, std::move(navigation_type), from);
}

// static
bindings::ScriptWrappable* NavigationActivation::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  // The first value of NavigationType, a Known Gap's default.
  return bindings::MakeForKnownGap<NavigationActivation>(
      info, bindings::V8NavigationActivation::kWrapperTypeInfo, nullptr,
      std::u16string(u"push"), nullptr);
}

NavigationActivation::NavigationActivation(NavigationHistoryEntry* entry,
                                           std::u16string navigation_type,
                                           NavigationHistoryEntry* from)
    : entry_(entry),
      from_(from),
      navigation_type_(std::move(navigation_type)) {}

NavigationActivation::~NavigationActivation() = default;

const bindings::WrapperTypeInfo* NavigationActivation::GetWrapperTypeInfo()
    const {
  return &bindings::V8NavigationActivation::kWrapperTypeInfo;
}

void NavigationActivation::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(entry_);
  visitor->Trace(from_);
}

}  // namespace mitto
