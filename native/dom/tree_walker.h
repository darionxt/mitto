// The DOM's TreeWalker, as Blink's TreeWalker walks a tree from its current
// node through the nodes that whatToShow and the filter accept.
//
// Upstream: third_party/blink/renderer/core/dom/tree_walker.cc

#ifndef MITTO_DOM_TREE_WALKER_H_
#define MITTO_DOM_TREE_WALKER_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/node_iterator_base.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class TreeWalker final : public bindings::ScriptWrappable,
                         public NodeIteratorBase {
 public:
  static TreeWalker* Create(Node* root,
                            uint32_t what_to_show,
                            v8::Local<v8::Object> filter);

  TreeWalker(v8::Isolate* isolate,
             Node* root,
             uint32_t what_to_show,
             v8::Local<v8::Object> filter);
  ~TreeWalker() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Node* CurrentNode() const { return current_.Get(); }
  void SetCurrentNode(Node* node) { current_ = node; }
  Node* ParentNode(bindings::ExceptionState& exception_state);
  Node* FirstChild(bindings::ExceptionState& exception_state);
  Node* LastChild(bindings::ExceptionState& exception_state);
  Node* PreviousSibling(bindings::ExceptionState& exception_state);
  Node* NextSibling(bindings::ExceptionState& exception_state);
  Node* PreviousNode(bindings::ExceptionState& exception_state);
  Node* NextNode(bindings::ExceptionState& exception_state);

 private:
  // The children of the current node, from the first or the last.
  Node* TraverseChildren(bool forward,
                         bindings::ExceptionState& exception_state);
  // Its siblings, the next or the previous ones.
  Node* TraverseSiblings(bool next, bindings::ExceptionState& exception_state);

  cppgc::Member<Node> current_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TREE_WALKER_H_
