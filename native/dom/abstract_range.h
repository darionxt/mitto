// The DOM's AbstractRange: two boundary points, each a node and an offset in
// it, which Range keeps live and StaticRange keeps as they were given. The
// objects of OpaqueRange, which has no class, inherit from it, so it is not
// final.
//
// Upstream: third_party/blink/renderer/core/dom/abstract_range.cc

#ifndef MITTO_DOM_ABSTRACT_RANGE_H_
#define MITTO_DOM_ABSTRACT_RANGE_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Node;

class AbstractRange : public bindings::ScriptWrappable {
 public:
  // One collapsed at the start of the current window's document, for a
  // Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  AbstractRange(Node* start_container,
                uint32_t start_offset,
                Node* end_container,
                uint32_t end_offset);
  ~AbstractRange() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Node* StartContainer() const { return start_container_.Get(); }
  uint32_t StartOffset() const { return start_offset_; }
  Node* EndContainer() const { return end_container_.Get(); }
  uint32_t EndOffset() const { return end_offset_; }
  bool Collapsed() const {
    return start_container_ == end_container_ && start_offset_ == end_offset_;
  }

  // The DOM standard's length of a node: its data's or its children's.
  static uint32_t LengthOfContents(const Node& node);

 protected:
  cppgc::Member<Node> start_container_;
  uint32_t start_offset_;
  cppgc::Member<Node> end_container_;
  uint32_t end_offset_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ABSTRACT_RANGE_H_
