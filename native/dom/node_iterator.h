// The DOM's NodeIterator, as Blink's NodeIterator walks a tree in order from
// a reference node, which the document moves when a node that holds it is
// removed (Document::NodeWillBeRemoved()).
//
// Upstream: third_party/blink/renderer/core/dom/node_iterator.cc

#ifndef MITTO_DOM_NODE_ITERATOR_H_
#define MITTO_DOM_NODE_ITERATOR_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/node_iterator_base.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class NodeIterator final : public bindings::ScriptWrappable,
                           public NodeIteratorBase {
 public:
  // Made through Document::CreateNodeIterator(), which attaches it to its
  // root's document.
  static NodeIterator* Create(Node* root,
                              uint32_t what_to_show,
                              v8::Local<v8::Object> filter);

  NodeIterator(v8::Isolate* isolate,
               Node* root,
               uint32_t what_to_show,
               v8::Local<v8::Object> filter);
  ~NodeIterator() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Node* ReferenceNode() const { return reference_.node.Get(); }
  bool PointerBeforeReferenceNode() const {
    return reference_.is_pointer_before_node;
  }
  Node* NextNode(bindings::ExceptionState& exception_state);
  Node* PreviousNode(bindings::ExceptionState& exception_state);
  void Detach() {}

  // The DOM standard's "NodeIterator pre-removing steps".
  void NodeWillBeRemoved(Node& removed);

 private:
  struct NodePointer {
    void Trace(cppgc::Visitor* visitor) const { visitor->Trace(node); }

    bool MoveToNext(Node* root);
    bool MoveToPrevious(Node* root);

    cppgc::Member<Node> node;
    bool is_pointer_before_node = true;
  };

  void UpdateForNodeRemoval(Node& removed, NodePointer& pointer) const;

  NodePointer reference_;
  // Where a call to nextNode() or previousNode() is, while its filter runs.
  NodePointer candidate_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_ITERATOR_H_
