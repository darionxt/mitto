#include "mitto/dom/node_iterator.h"

#include "mitto/bindings/v8_node_iterator.h"
#include "mitto/dom/attached_traversals.h"
#include "mitto/dom/document.h"
#include "mitto/dom/node.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// Blink's NodeTraversal::Previous(): the node before `node` in tree order,
// or null at `stay_within`.
Node* PreviousInTree(const Node& node, const Node* stay_within) {
  if (&node == stay_within) {
    return nullptr;
  }
  if (Node* previous = node.PreviousSibling()) {
    while (Node* const child = previous->LastChild()) {
      previous = child;
    }
    return previous;
  }
  return node.ParentNode();
}

bool IsDescendantOf(const Node& node, const Node& ancestor) {
  return &node != &ancestor && ancestor.IsInclusiveAncestorOf(node);
}

}  // namespace

bool NodeIterator::NodePointer::MoveToNext(Node* root) {
  if (!node) {
    return false;
  }
  if (is_pointer_before_node) {
    is_pointer_before_node = false;
    return true;
  }
  node = node->NextInTree(root);
  return !!node;
}

bool NodeIterator::NodePointer::MoveToPrevious(Node* root) {
  if (!node) {
    return false;
  }
  if (!is_pointer_before_node) {
    is_pointer_before_node = true;
    return true;
  }
  node = PreviousInTree(*node, root);
  return !!node;
}

// static
NodeIterator* NodeIterator::Create(Node* root,
                                   uint32_t what_to_show,
                                   v8::Local<v8::Object> filter) {
  NodeIterator* const iterator = bindings::MakeGarbageCollected<NodeIterator>(
      v8::Isolate::GetCurrent(), root, what_to_show, filter);
  root->GetDocument().Traversals().AttachNodeIterator(iterator);
  return iterator;
}

NodeIterator::NodeIterator(v8::Isolate* isolate,
                           Node* root,
                           uint32_t what_to_show,
                           v8::Local<v8::Object> filter)
    : NodeIteratorBase(isolate, root, what_to_show, filter) {
  reference_.node = root;
}

NodeIterator::~NodeIterator() = default;

const bindings::WrapperTypeInfo* NodeIterator::GetWrapperTypeInfo() const {
  return &bindings::V8NodeIterator::kWrapperTypeInfo;
}

void NodeIterator::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  NodeIteratorBase::Trace(visitor);
  reference_.Trace(visitor);
  candidate_.Trace(visitor);
}

Node* NodeIterator::NextNode(bindings::ExceptionState& exception_state) {
  Node* result = nullptr;
  candidate_ = reference_;
  while (candidate_.MoveToNext(Root())) {
    Node* const provisional = candidate_.node.Get();
    const std::optional<uint16_t> accepted =
        AcceptNode(provisional, exception_state);
    if (!accepted) {
      break;
    }
    if (*accepted == kFilterAccept) {
      reference_ = candidate_;
      result = provisional;
      break;
    }
  }
  candidate_.node = nullptr;
  return result;
}

Node* NodeIterator::PreviousNode(bindings::ExceptionState& exception_state) {
  Node* result = nullptr;
  candidate_ = reference_;
  while (candidate_.MoveToPrevious(Root())) {
    Node* const provisional = candidate_.node.Get();
    const std::optional<uint16_t> accepted =
        AcceptNode(provisional, exception_state);
    if (!accepted) {
      break;
    }
    if (*accepted == kFilterAccept) {
      reference_ = candidate_;
      result = provisional;
      break;
    }
  }
  candidate_.node = nullptr;
  return result;
}

void NodeIterator::NodeWillBeRemoved(Node& removed) {
  UpdateForNodeRemoval(removed, candidate_);
  UpdateForNodeRemoval(removed, reference_);
}

// Blink's UpdateForNodeRemoval(), which differs from the DOM standard where
// the removed node has no node after it.
void NodeIterator::UpdateForNodeRemoval(Node& removed,
                                        NodePointer& pointer) const {
  Node* const root = Root();
  if (!IsDescendantOf(removed, *root)) {
    return;
  }
  const bool removes_reference = &removed == pointer.node.Get();
  const bool removes_ancestor =
      pointer.node && IsDescendantOf(*pointer.node, removed);
  if (!removes_reference && !removes_ancestor) {
    return;
  }
  if (pointer.is_pointer_before_node) {
    Node* node = removed.NextInTree(root);
    if (node) {
      while (node && IsDescendantOf(*node, removed)) {
        node = node->NextInTree(root);
      }
      if (node) {
        pointer.node = node;
      }
    } else {
      node = PreviousInTree(removed, root);
      if (node) {
        if (removes_ancestor) {
          while (node && IsDescendantOf(*node, removed)) {
            node = PreviousInTree(*node, root);
          }
        }
        if (node) {
          pointer.node = node;
          pointer.is_pointer_before_node = false;
        }
      }
    }
    return;
  }
  Node* node = PreviousInTree(removed, root);
  if (node) {
    if (removes_ancestor) {
      while (node && IsDescendantOf(*node, removed)) {
        node = PreviousInTree(*node, root);
      }
    }
    if (node) {
      pointer.node = node;
    }
    return;
  }
  node = removed.NextInTree(root);
  if (removes_ancestor) {
    while (node && IsDescendantOf(*node, removed)) {
      node = PreviousInTree(*node, root);
    }
  }
  if (node) {
    pointer.node = node;
  }
}

}  // namespace mitto
