#include "mitto/dom/node_list.h"

#include "mitto/bindings/v8_node_list.h"
#include "mitto/dom/node.h"

namespace mitto {

// static
NodeList* NodeList::CreateChildNodes(Node* root) {
  return bindings::MakeGarbageCollected<NodeList>(root);
}

// static
NodeList* NodeList::CreateStatic(const std::vector<Node*>& nodes) {
  NodeList* const list = bindings::MakeGarbageCollected<NodeList>(nullptr);
  list->is_static_ = true;
  for (Node* node : nodes) {
    list->static_nodes_.push_back(node);
  }
  return list;
}

// static
bindings::ScriptWrappable* NodeList::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<NodeList>(
      info, bindings::V8NodeList::kWrapperTypeInfo, nullptr);
}

NodeList::NodeList(Node* root) : root_(root) {}

NodeList::~NodeList() = default;

const bindings::WrapperTypeInfo* NodeList::GetWrapperTypeInfo() const {
  return &bindings::V8NodeList::kWrapperTypeInfo;
}

void NodeList::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(root_);
  static_nodes_.Trace(visitor);
}

uint32_t NodeList::Length() const {
  if (is_static_) {
    return static_cast<uint32_t>(static_nodes_.size());
  }
  uint32_t length = 0;
  for (Node* child = root_ ? root_->FirstChild() : nullptr; child;
       child = child->NextSibling()) {
    ++length;
  }
  return length;
}

Node* NodeList::Item(uint32_t index) const {
  if (is_static_) {
    return index < static_nodes_.size() ? static_nodes_[index] : nullptr;
  }
  for (Node* child = root_ ? root_->FirstChild() : nullptr; child;
       child = child->NextSibling()) {
    if (index-- == 0) {
      return child;
    }
  }
  return nullptr;
}

}  // namespace mitto
