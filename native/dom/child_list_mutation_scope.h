// Gathers the changes to a node's children into the records that
// MutationObservers get, as Blink's ChildListMutationScope does: the changes
// that one operation makes, while a scope for the node is on the stack, go
// into one record while each continues where the last left off, so that
// replaceChildren() makes one record of every node it takes out and puts in.
//
// Upstream: third_party/blink/renderer/core/dom/child_list_mutation_scope.h
// Upstream: third_party/blink/renderer/core/dom/child_list_mutation_scope.cc

#ifndef MITTO_DOM_CHILD_LIST_MUTATION_SCOPE_H_
#define MITTO_DOM_CHILD_LIST_MUTATION_SCOPE_H_

#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class MutationObserverInterestGroup;
class Node;

// The record being gathered for one node, which every scope of the node
// shares.
class ChildListMutationAccumulator final
    : public cppgc::GarbageCollected<ChildListMutationAccumulator> {
 public:
  // The node's accumulator, or a new one if an observer is concerned, or
  // null.
  static ChildListMutationAccumulator* GetOrCreate(Node& target);

  ChildListMutationAccumulator(Node* target,
                               MutationObserverInterestGroup* observers);

  void Trace(cppgc::Visitor* visitor) const;

  void ChildAdded(Node& child);
  void WillRemoveChild(Node& child);
  void EnterMutationScope() { ++mutation_scopes_; }
  // Queues the record once the last scope of the node is left.
  void LeaveMutationScope();

 private:
  bool IsEmpty() const {
    return removed_nodes_.empty() && added_nodes_.empty();
  }
  bool IsAddedNodeInOrder(Node& child) const;
  bool IsRemovedNodeInOrder(Node& child) const;
  void EnqueueMutationRecord();

  cppgc::Member<Node> target_;
  HeapVector<Node> removed_nodes_;
  HeapVector<Node> added_nodes_;
  cppgc::Member<Node> previous_sibling_;
  cppgc::Member<Node> next_sibling_;
  cppgc::Member<Node> last_added_;
  cppgc::Member<MutationObserverInterestGroup> observers_;
  unsigned mutation_scopes_ = 0;
};

// Lives on the stack for one operation on a node's children.
class ChildListMutationScope final {
 public:
  explicit ChildListMutationScope(Node& target);
  ChildListMutationScope(const ChildListMutationScope&) = delete;
  ChildListMutationScope& operator=(const ChildListMutationScope&) = delete;
  ~ChildListMutationScope();

  // `child` was just put among the node's children.
  void ChildAdded(Node& child);
  // `child`, still among them, is about to leave.
  void WillRemoveChild(Node& child);

 private:
  const cppgc::Persistent<ChildListMutationAccumulator> accumulator_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CHILD_LIST_MUTATION_SCOPE_H_
