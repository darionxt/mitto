// Event Timing's EventCounts, performance.eventCounts: how many events of each
// type that it counts the document has dispatched, which for a Page is none of
// each, since nothing dispatches input. Its entries come in the order Blink's
// HashMap of them iterates, as Chrome 157 gives it.
//
// Upstream: third_party/blink/renderer/core/timing/event_counts.cc

#ifndef MITTO_DOM_EVENT_COUNTS_H_
#define MITTO_DOM_EVENT_COUNTS_H_

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class EventCounts final : public bindings::ScriptWrappable {
 public:
  static EventCounts* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  EventCounts();
  ~EventCounts() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The maplike's entries: each type and its count.
  std::vector<std::pair<std::u16string, uint64_t>> MaplikeEntries() const;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_COUNTS_H_
