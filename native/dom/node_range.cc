#include "mitto/dom/node_range.h"

#include "mitto/bindings/v8_node_range.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
bindings::ScriptWrappable* NodeRange::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  Document* const document = &Window::Current().GetDocument();
  return bindings::MakeForKnownGap<NodeRange>(
      info, bindings::V8NodeRange::kWrapperTypeInfo, document, 0u, document,
      0u);
}

NodeRange::NodeRange(Node* start_container,
                     uint32_t start_offset,
                     Node* end_container,
                     uint32_t end_offset)
    : AbstractRange(start_container, start_offset, end_container, end_offset) {}

NodeRange::~NodeRange() = default;

const bindings::WrapperTypeInfo* NodeRange::GetWrapperTypeInfo() const {
  return &bindings::V8NodeRange::kWrapperTypeInfo;
}

}  // namespace mitto
