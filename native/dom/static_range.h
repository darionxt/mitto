// The DOM's StaticRange, whose boundary points stay as they were given, as
// Blink's StaticRange.
//
// Upstream: third_party/blink/renderer/core/dom/static_range.cc

#ifndef MITTO_DOM_STATIC_RANGE_H_
#define MITTO_DOM_STATIC_RANGE_H_

#include <stdint.h>

#include "mitto/dom/node_range.h"

namespace mitto {

namespace bindings {
class ExceptionState;
struct StaticRangeInit;
}  // namespace bindings

class StaticRange final : public NodeRange {
 public:
  // new StaticRange().
  static StaticRange* Create(const bindings::StaticRangeInit& init,
                             bindings::ExceptionState& exception_state);
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  StaticRange(Node* start_container,
              uint32_t start_offset,
              Node* end_container,
              uint32_t end_offset);
  ~StaticRange() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_STATIC_RANGE_H_
