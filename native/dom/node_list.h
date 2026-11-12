// The DOM's NodeList: a node's childNodes, a live view of its children, as
// Blink's ChildNodeList reads them on each access. RadioNodeList, which has no
// class, inherits from it, so it is not final.
//
// Upstream: third_party/blink/renderer/core/dom/child_node_list.cc
// Upstream: third_party/blink/renderer/core/dom/node_list.h

#ifndef MITTO_DOM_NODE_LIST_H_
#define MITTO_DOM_NODE_LIST_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Node;

class NodeList : public bindings::ScriptWrappable {
 public:
  // The children of `root`.
  static NodeList* CreateChildNodes(Node* root);
  // An empty one, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit NodeList(Node* root);
  ~NodeList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  Node* Item(uint32_t index) const;

 private:
  cppgc::Member<Node> root_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_LIST_H_
