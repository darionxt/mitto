#include "mitto/dom/node_list.h"

#include "mitto/bindings/v8_node_list.h"
#include "mitto/dom/dom_tree_version.h"
#include "mitto/dom/element.h"
#include "mitto/dom/node.h"

namespace mitto {

// static
NodeList* NodeList::CreateChildNodes(Node* root) {
  return bindings::MakeGarbageCollected<NodeList>(Type::kChildNodes, root);
}

// static
NodeList* NodeList::CreateStatic(const NodeVector& nodes) {
  NodeList* const list =
      bindings::MakeGarbageCollected<NodeList>(Type::kStatic, nullptr);
  for (const cppgc::Persistent<Node>& node : nodes) {
    list->nodes_.push_back(node.Get());
  }
  return list;
}

// static
NodeList* NodeList::CreateName(Node* document, const std::u16string& name) {
  NodeList* const list =
      bindings::MakeGarbageCollected<NodeList>(Type::kName, document);
  list->name_ = name;
  return list;
}

// static
bindings::ScriptWrappable* NodeList::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<NodeList>(
      info, bindings::V8NodeList::kWrapperTypeInfo, Type::kStatic, nullptr);
}

NodeList::NodeList(Type type, Node* root) : type_(type), root_(root) {}

NodeList::~NodeList() = default;

const bindings::WrapperTypeInfo* NodeList::GetWrapperTypeInfo() const {
  return &bindings::V8NodeList::kWrapperTypeInfo;
}

void NodeList::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(root_);
  nodes_.Trace(visitor);
}

// Blink's NameNodeList::ElementMatches(), with
// GetElementsByNameOnlyHTMLElements, which is stable.
const HeapVector<Node>& NodeList::NamedElements() const {
  if (nodes_version_ == DomTreeVersion()) {
    return nodes_;
  }
  nodes_.clear();
  nodes_version_ = DomTreeVersion();
  for (Node* node = root_ ? root_->FirstChild() : nullptr; node;
       node = node->NextInTree(root_.Get())) {
    if (!node->IsElementNode()) {
      continue;
    }
    const auto& element = static_cast<const Element&>(*node);
    const std::u16string* name = element.FindAttribute(u"name");
    if (element.IsHTMLElement() && name && *name == name_) {
      nodes_.push_back(node);
    }
  }
  return nodes_;
}

uint32_t NodeList::Length() const {
  switch (type_) {
    case Type::kChildNodes: {
      uint32_t length = 0;
      for (Node* child = root_ ? root_->FirstChild() : nullptr; child;
           child = child->NextSibling()) {
        ++length;
      }
      return length;
    }
    case Type::kStatic:
      return static_cast<uint32_t>(nodes_.size());
    case Type::kName:
      return static_cast<uint32_t>(NamedElements().size());
  }
}

Node* NodeList::Item(uint32_t index) const {
  switch (type_) {
    case Type::kChildNodes:
      for (Node* child = root_ ? root_->FirstChild() : nullptr; child;
           child = child->NextSibling()) {
        if (index-- == 0) {
          return child;
        }
      }
      return nullptr;
    case Type::kStatic:
      return index < nodes_.size() ? nodes_[index] : nullptr;
    case Type::kName: {
      const HeapVector<Node>& nodes = NamedElements();
      return index < nodes.size() ? nodes[index] : nullptr;
    }
  }
}

}  // namespace mitto
