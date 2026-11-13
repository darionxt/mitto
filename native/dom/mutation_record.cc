#include "mitto/dom/mutation_record.h"

#include "base/notreached.h"
#include "mitto/bindings/v8_mutation_record.h"
#include "mitto/dom/node.h"
#include "mitto/dom/node_list.h"

namespace mitto {

// static
MutationRecord* MutationRecord::CreateChildList(Node* target,
                                                NodeList* added_nodes,
                                                NodeList* removed_nodes,
                                                Node* previous_sibling,
                                                Node* next_sibling) {
  return bindings::MakeGarbageCollected<MutationRecord>(
      Kind::kChildList, target, added_nodes, removed_nodes, previous_sibling,
      next_sibling, std::nullopt, std::nullopt, nullptr);
}

// static
MutationRecord* MutationRecord::CreateAttributes(
    Node* target,
    const std::u16string& name,
    const std::optional<std::u16string>& old_value) {
  return bindings::MakeGarbageCollected<MutationRecord>(
      Kind::kAttributes, target, nullptr, nullptr, nullptr, nullptr, name,
      old_value, nullptr);
}

// static
MutationRecord* MutationRecord::CreateCharacterData(
    Node* target,
    const std::u16string& old_value) {
  return bindings::MakeGarbageCollected<MutationRecord>(
      Kind::kCharacterData, target, nullptr, nullptr, nullptr, nullptr,
      std::nullopt, old_value, nullptr);
}

// static
MutationRecord* MutationRecord::CreateWithNullOldValue(MutationRecord* record) {
  return bindings::MakeGarbageCollected<MutationRecord>(
      record->kind_, nullptr, nullptr, nullptr, nullptr, nullptr, std::nullopt,
      std::nullopt, record);
}

MutationRecord::MutationRecord(
    Kind kind,
    Node* target,
    NodeList* added_nodes,
    NodeList* removed_nodes,
    Node* previous_sibling,
    Node* next_sibling,
    const std::optional<std::u16string>& attribute_name,
    const std::optional<std::u16string>& old_value,
    MutationRecord* record_with_old_value)
    : kind_(kind),
      target_(target),
      added_nodes_(added_nodes),
      removed_nodes_(removed_nodes),
      previous_sibling_(previous_sibling),
      next_sibling_(next_sibling),
      attribute_name_(attribute_name),
      old_value_(old_value),
      record_with_old_value_(record_with_old_value) {}

MutationRecord::~MutationRecord() = default;

const bindings::WrapperTypeInfo* MutationRecord::GetWrapperTypeInfo() const {
  return &bindings::V8MutationRecord::kWrapperTypeInfo;
}

void MutationRecord::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(target_);
  visitor->Trace(added_nodes_);
  visitor->Trace(removed_nodes_);
  visitor->Trace(previous_sibling_);
  visitor->Trace(next_sibling_);
  visitor->Trace(record_with_old_value_);
}

std::u16string MutationRecord::Type() const {
  switch (kind_) {
    case Kind::kChildList:
      return u"childList";
    case Kind::kAttributes:
      return u"attributes";
    case Kind::kCharacterData:
      return u"characterData";
  }
  NOTREACHED();
}

Node* MutationRecord::Target() const {
  if (record_with_old_value_) {
    return record_with_old_value_->Target();
  }
  return target_.Get();
}

NodeList* MutationRecord::AddedNodes() {
  if (record_with_old_value_) {
    return record_with_old_value_->AddedNodes();
  }
  if (!added_nodes_) {
    added_nodes_ = NodeList::CreateStatic({});
  }
  return added_nodes_.Get();
}

NodeList* MutationRecord::RemovedNodes() {
  if (record_with_old_value_) {
    return record_with_old_value_->RemovedNodes();
  }
  if (!removed_nodes_) {
    removed_nodes_ = NodeList::CreateStatic({});
  }
  return removed_nodes_.Get();
}

Node* MutationRecord::PreviousSibling() const {
  if (record_with_old_value_) {
    return record_with_old_value_->PreviousSibling();
  }
  return previous_sibling_.Get();
}

Node* MutationRecord::NextSibling() const {
  if (record_with_old_value_) {
    return record_with_old_value_->NextSibling();
  }
  return next_sibling_.Get();
}

std::optional<std::u16string> MutationRecord::AttributeName() const {
  if (record_with_old_value_) {
    return record_with_old_value_->AttributeName();
  }
  return attribute_name_;
}

std::optional<std::u16string> MutationRecord::AttributeNamespace() const {
  // An attribute of the Runtime has no namespace: the methods that name one
  // are Known Gaps.
  return std::nullopt;
}

std::optional<std::u16string> MutationRecord::OldValue() const {
  if (record_with_old_value_) {
    return std::nullopt;
  }
  return old_value_;
}

}  // namespace mitto
