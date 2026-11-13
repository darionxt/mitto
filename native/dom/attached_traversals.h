// The live ranges and node iterators of a document, held weakly, which the
// DOM tells each time the document's trees change, so that each keeps its
// boundary points or reference node where the DOM standard puts them, as
// Blink's Document keeps its attached ranges and node iterators.
//
// Upstream: third_party/blink/renderer/core/dom/document.cc

#ifndef MITTO_DOM_ATTACHED_TRAVERSALS_H_
#define MITTO_DOM_ATTACHED_TRAVERSALS_H_

#include <stdint.h>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class CharacterData;
class Document;
class Node;
class NodeIterator;
class Range;
class Text;

class AttachedTraversals final
    : public cppgc::GarbageCollected<AttachedTraversals> {
 public:
  AttachedTraversals();
  ~AttachedTraversals();

  void Trace(cppgc::Visitor* visitor) const;

  void AttachRange(Range* range);
  void DetachRange(Range* range);
  void AttachNodeIterator(NodeIterator* iterator);

  // `node`, which has a parent, is about to be removed from it.
  void NodeWillBeRemoved(Node& node);
  // `node` was inserted into its parent.
  void DidInsertNode(Node& node);
  // `count` code units of `node`'s data from `offset` were replaced with
  // `added` code units.
  void DidReplaceText(CharacterData& node,
                      uint32_t offset,
                      uint32_t count,
                      uint32_t added);
  // `new_node`, the part of `node` from `offset` on, was inserted after it,
  // and `node` still holds all its data.
  void DidSplitText(Text& node, Text& new_node, uint32_t offset);
  // `next`, the Text node after `node`, is about to be merged into it, whose
  // data was `offset` long and now ends with `next`'s.
  void DidMergeText(Text& node, Text& next, uint32_t offset);
  // `root` and what is inside it moved from this document to `document`:
  // the iterators rooted there and the ranges now in it go with them, as
  // Blink's MoveNodeIteratorsToNewDocument() and DidMoveTreeToNewDocument()
  // move them.
  void DidMoveTreeToNewDocument(Node& root, Document& document);

 private:
  struct Entry;

  // Calls `visit` with each range or iterator a page still keeps, and drops
  // the entries of those it does not.
  template <typename Visit>
  void ForEach(Visit visit);

  cppgc::Member<Entry> first_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ATTACHED_TRAVERSALS_H_
