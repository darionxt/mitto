#include "mitto/dom/tree_walker.h"

#include "mitto/bindings/v8_tree_walker.h"
#include "mitto/dom/node.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
TreeWalker* TreeWalker::Create(Node* root,
                               uint32_t what_to_show,
                               v8::Local<v8::Object> filter) {
  return bindings::MakeGarbageCollected<TreeWalker>(v8::Isolate::GetCurrent(),
                                                    root, what_to_show, filter);
}

TreeWalker::TreeWalker(v8::Isolate* isolate,
                       Node* root,
                       uint32_t what_to_show,
                       v8::Local<v8::Object> filter)
    : NodeIteratorBase(isolate, root, what_to_show, filter), current_(root) {}

TreeWalker::~TreeWalker() = default;

const bindings::WrapperTypeInfo* TreeWalker::GetWrapperTypeInfo() const {
  return &bindings::V8TreeWalker::kWrapperTypeInfo;
}

void TreeWalker::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  NodeIteratorBase::Trace(visitor);
  visitor->Trace(current_);
}

Node* TreeWalker::ParentNode(bindings::ExceptionState& exception_state) {
  Node* node = current_.Get();
  while (node != Root()) {
    node = node->ParentNode();
    if (!node) {
      return nullptr;
    }
    const std::optional<uint16_t> result = AcceptNode(node, exception_state);
    if (!result) {
      return nullptr;
    }
    if (*result == kFilterAccept) {
      current_ = node;
      return node;
    }
  }
  return nullptr;
}

Node* TreeWalker::TraverseChildren(bool forward,
                                   bindings::ExceptionState& exception_state) {
  Node* node = forward ? current_->FirstChild() : current_->LastChild();
  while (node) {
    const std::optional<uint16_t> result = AcceptNode(node, exception_state);
    if (!result) {
      return nullptr;
    }
    if (*result == kFilterAccept) {
      current_ = node;
      return node;
    }
    if (*result == kFilterSkip) {
      if (Node* const child =
              forward ? node->FirstChild() : node->LastChild()) {
        node = child;
        continue;
      }
    }
    while (true) {
      if (Node* const sibling =
              forward ? node->NextSibling() : node->PreviousSibling()) {
        node = sibling;
        break;
      }
      Node* const parent = node->ParentNode();
      if (!parent || parent == Root() || parent == current_) {
        return nullptr;
      }
      node = parent;
    }
  }
  return nullptr;
}

Node* TreeWalker::FirstChild(bindings::ExceptionState& exception_state) {
  return TraverseChildren(/*forward=*/true, exception_state);
}

Node* TreeWalker::LastChild(bindings::ExceptionState& exception_state) {
  return TraverseChildren(/*forward=*/false, exception_state);
}

Node* TreeWalker::TraverseSiblings(bool next,
                                   bindings::ExceptionState& exception_state) {
  Node* node = current_.Get();
  if (node == Root()) {
    return nullptr;
  }
  while (true) {
    Node* sibling = next ? node->NextSibling() : node->PreviousSibling();
    while (sibling) {
      node = sibling;
      const std::optional<uint16_t> result = AcceptNode(node, exception_state);
      if (!result) {
        return nullptr;
      }
      if (*result == kFilterAccept) {
        current_ = node;
        return node;
      }
      sibling = next ? sibling->FirstChild() : sibling->LastChild();
      if (*result == kFilterReject || !sibling) {
        sibling = next ? node->NextSibling() : node->PreviousSibling();
      }
    }
    node = node->ParentNode();
    if (!node || node == Root()) {
      return nullptr;
    }
    const std::optional<uint16_t> result = AcceptNode(node, exception_state);
    if (!result || *result == kFilterAccept) {
      return nullptr;
    }
  }
}

Node* TreeWalker::PreviousSibling(bindings::ExceptionState& exception_state) {
  return TraverseSiblings(/*next=*/false, exception_state);
}

Node* TreeWalker::NextSibling(bindings::ExceptionState& exception_state) {
  return TraverseSiblings(/*next=*/true, exception_state);
}

Node* TreeWalker::PreviousNode(bindings::ExceptionState& exception_state) {
  Node* node = current_.Get();
  while (node != Root()) {
    while (Node* const previous_sibling = node->PreviousSibling()) {
      node = previous_sibling;
      std::optional<uint16_t> result = AcceptNode(node, exception_state);
      if (!result) {
        return nullptr;
      }
      if (*result == kFilterReject) {
        continue;
      }
      while (Node* const last_child = node->LastChild()) {
        node = last_child;
        result = AcceptNode(node, exception_state);
        if (!result) {
          return nullptr;
        }
        if (*result == kFilterReject) {
          break;
        }
      }
      if (*result == kFilterAccept) {
        current_ = node;
        return node;
      }
    }
    if (node == Root()) {
      return nullptr;
    }
    Node* const parent = node->ParentNode();
    if (!parent) {
      return nullptr;
    }
    node = parent;
    const std::optional<uint16_t> result = AcceptNode(node, exception_state);
    if (!result) {
      return nullptr;
    }
    if (*result == kFilterAccept) {
      current_ = node;
      return node;
    }
  }
  return nullptr;
}

Node* TreeWalker::NextNode(bindings::ExceptionState& exception_state) {
  Node* node = current_.Get();
  while (true) {
    bool skipped = false;
    while (Node* const first_child = node->FirstChild()) {
      node = first_child;
      const std::optional<uint16_t> result = AcceptNode(node, exception_state);
      if (!result) {
        return nullptr;
      }
      if (*result == kFilterAccept) {
        current_ = node;
        return node;
      }
      if (*result == kFilterReject) {
        break;
      }
    }
    while (Node* const next = node->NextInTreeSkippingChildren(Root())) {
      node = next;
      const std::optional<uint16_t> result = AcceptNode(node, exception_state);
      if (!result) {
        return nullptr;
      }
      if (*result == kFilterAccept) {
        current_ = node;
        return node;
      }
      if (*result == kFilterSkip) {
        skipped = true;
        break;
      }
    }
    if (!skipped) {
      return nullptr;
    }
  }
}

}  // namespace mitto
