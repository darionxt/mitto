// The DOM's NodeRange, the AbstractRange whose boundary points are nodes
// that a page reads, which Range and StaticRange are.
//
// Upstream: third_party/blink/renderer/core/dom/node_range.idl

#ifndef MITTO_DOM_NODE_RANGE_H_
#define MITTO_DOM_NODE_RANGE_H_

#include <stdint.h>

#include "mitto/dom/abstract_range.h"

namespace mitto {

class NodeRange : public AbstractRange {
 public:
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NodeRange(Node* start_container,
            uint32_t start_offset,
            Node* end_container,
            uint32_t end_offset);
  ~NodeRange() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_RANGE_H_
