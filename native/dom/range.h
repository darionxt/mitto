// The DOM's Range: a live range, whose boundary points the document moves as
// its trees change (attached_traversals.h), with Blink's checks, messages and
// order of steps, and createContextualFragment() of DOM Parsing. Its
// geometry, getClientRects() and getBoundingClientRect(), needs layout and
// stays a Known Gap (roadmap, M9), and so does expand(), which needs it too.
//
// Upstream: third_party/blink/renderer/core/dom/range.cc
// Upstream: third_party/blink/renderer/core/editing/serializers/serialization.cc

#ifndef MITTO_DOM_RANGE_H_
#define MITTO_DOM_RANGE_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/node.h"
#include "mitto/dom/node_range.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CharacterData;
class Document;
class DocumentFragment;
class Text;

class Range final : public NodeRange {
 public:
  // compareBoundaryPoints()'s ways.
  enum CompareHow : uint16_t {
    kStartToStart = 0,
    kStartToEnd = 1,
    kEndToEnd = 2,
    kEndToStart = 3,
  };

  // new Range(), in the current window's document.
  static Range* Create();
  // One collapsed at the start of `document`, as createRange() gives.
  static Range* Create(Document& document);
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Range(Document& document);
  ~Range() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Node* CommonAncestorContainer() const;
  void SetStart(Node* node,
                uint32_t offset,
                bindings::ExceptionState& exception_state);
  void SetEnd(Node* node,
              uint32_t offset,
              bindings::ExceptionState& exception_state);
  void SetStartBefore(Node* node, bindings::ExceptionState& exception_state);
  void SetStartAfter(Node* node, bindings::ExceptionState& exception_state);
  void SetEndBefore(Node* node, bindings::ExceptionState& exception_state);
  void SetEndAfter(Node* node, bindings::ExceptionState& exception_state);
  void Collapse(bool to_start);
  void SelectNode(Node* node, bindings::ExceptionState& exception_state);
  void SelectNodeContents(Node* node,
                          bindings::ExceptionState& exception_state);
  int16_t CompareBoundaryPoints(
      uint16_t how,
      Range* source,
      bindings::ExceptionState& exception_state) const;
  void DeleteContents(bindings::ExceptionState& exception_state);
  DocumentFragment* ExtractContents(bindings::ExceptionState& exception_state);
  DocumentFragment* CloneContents(bindings::ExceptionState& exception_state);
  void InsertNode(Node* node, bindings::ExceptionState& exception_state);
  void SurroundContents(Node* new_parent,
                        bindings::ExceptionState& exception_state);
  Range* CloneRange() const;
  void Detach() {}
  bool IsPointInRange(Node* node,
                      uint32_t offset,
                      bindings::ExceptionState& exception_state) const;
  int16_t ComparePoint(Node* node,
                       uint32_t offset,
                       bindings::ExceptionState& exception_state) const;
  bool IntersectsNode(Node* node,
                      bindings::ExceptionState& exception_state) const;
  std::u16string ToString() const;
  DocumentFragment* CreateContextualFragment(
      const std::u16string& fragment,
      bindings::ExceptionState& exception_state);

  // What the document tells it, as the DOM standard's live range steps.
  void NodeWillBeRemoved(Node& node);
  void DidInsertNode(Node& node);
  void DidReplaceText(CharacterData& node,
                      uint32_t offset,
                      uint32_t count,
                      uint32_t added);
  void DidSplitText(Text& node, Text& new_node, uint32_t offset);
  void DidMergeText(Text& node, Text& next, uint32_t offset);

 private:
  enum class Action {
    kDelete,
    kExtract,
    kClone,
  };

  // Moves the range into `document`, collapsed at its start.
  void SetDocument(Document& document);
  // Blink's CheckNodeWOffset(): throws for a boundary point that cannot be.
  void CheckNodeWithOffset(Node* node,
                           uint32_t offset,
                           bindings::ExceptionState& exception_state) const;
  // Blink's CheckNodeBA(): throws for a node that setStartBefore() and the
  // like cannot take.
  void CheckNodeBeforeOrAfter(Node* node,
                              bindings::ExceptionState& exception_state) const;
  void CollapseIfNeeded(bool moved_document, bool collapse_to_start);
  // Whether `node` is in the document and tree of the range.
  bool HasSameRoot(const Node& node) const;
  void CheckExtractPrecondition(bindings::ExceptionState& exception_state);
  // The first node in the range, and the node after the last, in tree order.
  Node* FirstNode() const;
  Node* PastLastNode() const;

  DocumentFragment* ProcessContents(Action action,
                                    bindings::ExceptionState& exception_state);
  Node* ProcessContentsBetweenOffsets(
      Action action,
      DocumentFragment* fragment,
      Node* container,
      uint32_t start_offset,
      uint32_t end_offset,
      bindings::ExceptionState& exception_state);
  void ProcessNodes(Action action,
                    const NodeVector& nodes,
                    Node* old_container,
                    Node* new_container,
                    bindings::ExceptionState& exception_state);
  Node* ProcessAncestorsAndTheirSiblings(
      Action action,
      Node* container,
      bool forward,
      Node* cloned_container,
      Node* common_root,
      bindings::ExceptionState& exception_state);

  cppgc::Member<Document> owner_document_;
};

}  // namespace mitto

#endif  // MITTO_DOM_RANGE_H_
