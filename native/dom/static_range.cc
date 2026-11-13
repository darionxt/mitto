#include "mitto/dom/static_range.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_static_range.h"
#include "mitto/dom/document.h"
#include "mitto/dom/node.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
StaticRange* StaticRange::Create(const bindings::StaticRangeInit& init,
                                 bindings::ExceptionState& exception_state) {
  const auto is_refused = [](const Node* node) {
    return node->GetNodeType() == Node::kDocumentTypeNode ||
           node->GetNodeType() == Node::kAttributeNode;
  };
  // The bindings checked that the required members are there.
  Node* const start_container = *init.start_container;
  Node* const end_container = *init.end_container;
  if (is_refused(start_container) || is_refused(end_container)) {
    exception_state.ThrowDOMException(
        "InvalidNodeTypeError",
        "Neither startContainer nor endContainer can be a DocumentType or "
        "Attribute node.");
    return nullptr;
  }
  return bindings::MakeGarbageCollected<StaticRange>(
      start_container, *init.start_offset, end_container, *init.end_offset);
}

// static
bindings::ScriptWrappable* StaticRange::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  Document* const document = &Window::Current().GetDocument();
  return bindings::MakeForKnownGap<StaticRange>(
      info, bindings::V8StaticRange::kWrapperTypeInfo, document, 0u, document,
      0u);
}

StaticRange::StaticRange(Node* start_container,
                         uint32_t start_offset,
                         Node* end_container,
                         uint32_t end_offset)
    : NodeRange(start_container, start_offset, end_container, end_offset) {}

StaticRange::~StaticRange() = default;

const bindings::WrapperTypeInfo* StaticRange::GetWrapperTypeInfo() const {
  return &bindings::V8StaticRange::kWrapperTypeInfo;
}

}  // namespace mitto
