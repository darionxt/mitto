// What TreeWalker and NodeIterator share: their root, whatToShow and filter,
// and the DOM standard's "filter" algorithm, which calls the filter as Web
// IDL calls a callback interface's operation, as Blink's NodeIteratorBase
// does.
//
// Upstream: third_party/blink/renderer/core/dom/node_iterator_base.cc

#ifndef MITTO_DOM_NODE_ITERATOR_BASE_H_
#define MITTO_DOM_NODE_ITERATOR_BASE_H_

#include <stdint.h>

#include <optional>

#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Node;

class NodeIteratorBase {
 public:
  // NodeFilter's results.
  static constexpr uint16_t kFilterAccept = 1;
  static constexpr uint16_t kFilterReject = 2;
  static constexpr uint16_t kFilterSkip = 3;

  NodeIteratorBase(const NodeIteratorBase&) = delete;
  NodeIteratorBase& operator=(const NodeIteratorBase&) = delete;

  // The members of the interfaces.
  Node* Root() const { return root_.Get(); }
  uint32_t WhatToShow() const { return what_to_show_; }
  // Empty for no filter.
  v8::Local<v8::Object> Filter() const;

 protected:
  NodeIteratorBase(v8::Isolate* isolate,
                   Node* root,
                   uint32_t what_to_show,
                   v8::Local<v8::Object> filter);
  ~NodeIteratorBase();

  void Trace(cppgc::Visitor* visitor) const;

  // The DOM standard's "filter" of `node`, or nullopt if it threw, in which
  // case the exception is pending.
  std::optional<uint16_t> AcceptNode(Node* node,
                                     bindings::ExceptionState& exception_state);

 private:
  const cppgc::Member<Node> root_;
  const uint32_t what_to_show_;
  v8::TracedReference<v8::Object> filter_;
  // The DOM standard's active flag, set while the filter runs.
  bool active_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_ITERATOR_BASE_H_
