#include "mitto/dom/visibility_state_entry.h"

#include <utility>

#include "mitto/bindings/v8_visibility_state_entry.h"

namespace mitto {

VisibilityStateEntry::VisibilityStateEntry(std::u16string name,
                                           double start_time,
                                           uint64_t navigation_id)
    : PerformanceEntry(std::move(name), start_time, start_time, navigation_id) {
}

VisibilityStateEntry::~VisibilityStateEntry() = default;

const bindings::WrapperTypeInfo* VisibilityStateEntry::GetWrapperTypeInfo()
    const {
  return &bindings::V8VisibilityStateEntry::kWrapperTypeInfo;
}

PerformanceEntry::Kind VisibilityStateEntry::GetKind() const {
  return kVisibilityState;
}

std::u16string_view VisibilityStateEntry::TypeName() const {
  return u"visibility-state";
}

}  // namespace mitto
