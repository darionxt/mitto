// The DOM's NodeList: a node's childNodes, a live view of its children, as
// Blink's ChildNodeList reads them on each access; the static list that
// querySelectorAll() gives, as Blink's StaticNodeList; and the live list of
// getElementsByName(), as Blink's NameNodeList, which keeps the elements it
// found until a tree changes (dom_tree_version.h). RadioNodeList, which has
// no class, inherits from it, so it is not final.
//
// Upstream: third_party/blink/renderer/core/dom/child_node_list.cc
// Upstream: third_party/blink/renderer/core/dom/node_list.h
// Upstream: third_party/blink/renderer/core/dom/static_node_list.h
// Upstream: third_party/blink/renderer/core/dom/name_node_list.cc

#ifndef MITTO_DOM_NODE_LIST_H_
#define MITTO_DOM_NODE_LIST_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/node.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class NodeList : public bindings::ScriptWrappable {
 public:
  enum class Type {
    kChildNodes,
    kStatic,
    kName,
  };

  // The children of `root`.
  static NodeList* CreateChildNodes(Node* root);
  // `nodes`, which the list keeps as they are.
  static NodeList* CreateStatic(const NodeVector& nodes);
  // The HTML elements under `document` whose name attribute is `name`.
  static NodeList* CreateName(Node* document, const std::u16string& name);
  // An empty one, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NodeList(Type type, Node* root);
  ~NodeList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  Node* Item(uint32_t index) const;

  Type type() const { return type_; }
  const std::u16string& name() const { return name_; }

 private:
  // The elements of a name list, found again if a tree changed since the
  // last time.
  const HeapVector<Node>& NamedElements() const;

  const Type type_;
  cppgc::Member<Node> root_;
  std::u16string name_;
  // A static list's nodes, or the elements a name list found.
  mutable HeapVector<Node> nodes_;
  mutable std::optional<uint64_t> nodes_version_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_LIST_H_
