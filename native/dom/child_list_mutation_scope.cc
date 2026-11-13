#include "mitto/dom/child_list_mutation_scope.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/no_destructor.h"
#include "mitto/dom/mutation_observer.h"
#include "mitto/dom/mutation_record.h"
#include "mitto/dom/node.h"
#include "mitto/dom/node_list.h"

namespace mitto {

namespace {

// Blink's accumulator map: one accumulator for a node while any scope of it is
// on the stack, empty while none is.
using AccumulatorMap = std::vector<
    std::pair<const Node*, cppgc::Persistent<ChildListMutationAccumulator>>>;

AccumulatorMap& GetAccumulatorMap() {
  static base::NoDestructor<AccumulatorMap> map;
  return *map;
}

}  // namespace

// static
ChildListMutationAccumulator* ChildListMutationAccumulator::GetOrCreate(
    Node& target) {
  AccumulatorMap& map = GetAccumulatorMap();
  for (const auto& [node, accumulator] : map) {
    if (node == &target) {
      return accumulator.Get();
    }
  }
  MutationObserverInterestGroup* const observers =
      MutationObserverInterestGroup::CreateForChildListMutation(target);
  if (!observers) {
    return nullptr;
  }
  auto* const accumulator =
      bindings::MakeGarbageCollected<ChildListMutationAccumulator>(&target,
                                                                   observers);
  map.emplace_back(&target, accumulator);
  return accumulator;
}

ChildListMutationAccumulator::ChildListMutationAccumulator(
    Node* target,
    MutationObserverInterestGroup* observers)
    : target_(target), observers_(observers) {}

void ChildListMutationAccumulator::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(target_);
  removed_nodes_.Trace(visitor);
  added_nodes_.Trace(visitor);
  visitor->Trace(previous_sibling_);
  visitor->Trace(next_sibling_);
  visitor->Trace(last_added_);
  visitor->Trace(observers_);
}

void ChildListMutationAccumulator::LeaveMutationScope() {
  CHECK_GT(mutation_scopes_, 0u);
  if (--mutation_scopes_) {
    return;
  }
  if (!IsEmpty()) {
    EnqueueMutationRecord();
  }
  AccumulatorMap& map = GetAccumulatorMap();
  std::erase_if(map,
                [this](const auto& item) { return item.second.Get() == this; });
}

bool ChildListMutationAccumulator::IsAddedNodeInOrder(Node& child) const {
  return IsEmpty() || (last_added_.Get() == child.PreviousSibling() &&
                       next_sibling_.Get() == child.NextSibling());
}

void ChildListMutationAccumulator::ChildAdded(Node& child) {
  if (!IsAddedNodeInOrder(child)) {
    EnqueueMutationRecord();
  }
  if (IsEmpty()) {
    previous_sibling_ = child.PreviousSibling();
    next_sibling_ = child.NextSibling();
  }
  last_added_ = &child;
  added_nodes_.push_back(&child);
}

bool ChildListMutationAccumulator::IsRemovedNodeInOrder(Node& child) const {
  return IsEmpty() || next_sibling_.Get() == &child;
}

void ChildListMutationAccumulator::WillRemoveChild(Node& child) {
  if (!added_nodes_.empty() || !IsRemovedNodeInOrder(child)) {
    EnqueueMutationRecord();
  }
  if (IsEmpty()) {
    previous_sibling_ = child.PreviousSibling();
    next_sibling_ = child.NextSibling();
    last_added_ = child.PreviousSibling();
  } else {
    next_sibling_ = child.NextSibling();
  }
  removed_nodes_.push_back(&child);
}

void ChildListMutationAccumulator::EnqueueMutationRecord() {
  CHECK(!IsEmpty());
  NodeList* const added = NodeList::CreateStatic(added_nodes_.ToVector());
  const cppgc::Persistent<NodeList> held_added(added);
  NodeList* const removed = NodeList::CreateStatic(removed_nodes_.ToVector());
  const cppgc::Persistent<NodeList> held_removed(removed);
  MutationRecord* const record = MutationRecord::CreateChildList(
      target_.Get(), added, removed, previous_sibling_.Get(),
      next_sibling_.Get());
  added_nodes_.clear();
  removed_nodes_.clear();
  previous_sibling_ = nullptr;
  next_sibling_ = nullptr;
  last_added_ = nullptr;
  observers_->EnqueueMutationRecord(record);
}

ChildListMutationScope::ChildListMutationScope(Node& target)
    : accumulator_(ChildListMutationAccumulator::GetOrCreate(target)) {
  if (accumulator_) {
    accumulator_->EnterMutationScope();
  }
}

ChildListMutationScope::~ChildListMutationScope() {
  if (accumulator_) {
    accumulator_->LeaveMutationScope();
  }
}

void ChildListMutationScope::ChildAdded(Node& child) {
  if (accumulator_) {
    accumulator_->ChildAdded(child);
  }
}

void ChildListMutationScope::WillRemoveChild(Node& child) {
  if (accumulator_) {
    accumulator_->WillRemoveChild(child);
  }
}

}  // namespace mitto
