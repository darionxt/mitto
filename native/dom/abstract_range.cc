#include "mitto/dom/abstract_range.h"

#include "mitto/bindings/v8_abstract_range.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/node.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
bindings::ScriptWrappable* AbstractRange::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  Document* const document = &Window::Current().GetDocument();
  return bindings::MakeForKnownGap<AbstractRange>(
      info, bindings::V8AbstractRange::kWrapperTypeInfo, document, 0u, document,
      0u);
}

AbstractRange::AbstractRange(Node* start_container,
                             uint32_t start_offset,
                             Node* end_container,
                             uint32_t end_offset)
    : start_container_(start_container),
      start_offset_(start_offset),
      end_container_(end_container),
      end_offset_(end_offset) {}

AbstractRange::~AbstractRange() = default;

const bindings::WrapperTypeInfo* AbstractRange::GetWrapperTypeInfo() const {
  return &bindings::V8AbstractRange::kWrapperTypeInfo;
}

void AbstractRange::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(start_container_);
  visitor->Trace(end_container_);
}

// static
uint32_t AbstractRange::LengthOfContents(const Node& node) {
  switch (node.GetNodeType()) {
    case Node::kTextNode:
    case Node::kCdataSectionNode:
    case Node::kCommentNode:
    case Node::kProcessingInstructionNode:
      return static_cast<const CharacterData&>(node).Length();
    case Node::kElementNode:
    case Node::kDocumentNode:
    case Node::kDocumentFragmentNode: {
      uint32_t count = 0;
      for (const Node* child = node.FirstChild(); child;
           child = child->NextSibling()) {
        ++count;
      }
      return count;
    }
    default:
      return 0;
  }
}

}  // namespace mitto
