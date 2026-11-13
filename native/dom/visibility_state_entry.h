// Page Visibility's VisibilityStateEntry: the entry of the page's visibility
// that Blink adds when it makes a window's Performance, which for a Page is
// always visible, as its document says.
//
// Upstream: third_party/blink/renderer/core/timing/visibility_state_entry.cc
// Upstream: third_party/blink/renderer/core/timing/window_performance.cc

#ifndef MITTO_DOM_VISIBILITY_STATE_ENTRY_H_
#define MITTO_DOM_VISIBILITY_STATE_ENTRY_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/performance_entry.h"

namespace mitto {

class VisibilityStateEntry final : public PerformanceEntry {
 public:
  VisibilityStateEntry(std::u16string name,
                       double start_time,
                       uint64_t navigation_id);
  ~VisibilityStateEntry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // PerformanceEntry:
  Kind GetKind() const override;
  std::u16string_view TypeName() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_VISIBILITY_STATE_ENTRY_H_
