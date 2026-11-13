// The DOM's MutationRecord: one change that a MutationObserver hears of, to a
// node's children, attributes or data, as Blink's MutationRecord holds it.
//
// Upstream: third_party/blink/renderer/core/dom/mutation_record.h
// Upstream: third_party/blink/renderer/core/dom/mutation_record.cc

#ifndef MITTO_DOM_MUTATION_RECORD_H_
#define MITTO_DOM_MUTATION_RECORD_H_

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Node;
class NodeList;

class MutationRecord final : public bindings::ScriptWrappable {
 public:
  enum class Kind { kChildList, kAttributes, kCharacterData };

  // A change to `target`'s children: the nodes it gained and lost, between
  // `previous_sibling` and `next_sibling`.
  static MutationRecord* CreateChildList(Node* target,
                                         NodeList* added_nodes,
                                         NodeList* removed_nodes,
                                         Node* previous_sibling,
                                         Node* next_sibling);
  // A change to `target`'s attribute `name`, with no namespace, which had
  // `old_value`, null if it had none.
  static MutationRecord* CreateAttributes(
      Node* target,
      const std::u16string& name,
      const std::optional<std::u16string>& old_value);
  // A change to `target`'s data, which was `old_value`.
  static MutationRecord* CreateCharacterData(Node* target,
                                             const std::u16string& old_value);
  // `record` with a null oldValue, for an observer that did not ask for it,
  // as Blink's MutationRecordWithNullOldValue: the same nodes and node lists.
  static MutationRecord* CreateWithNullOldValue(MutationRecord* record);

  MutationRecord(Kind kind,
                 Node* target,
                 NodeList* added_nodes,
                 NodeList* removed_nodes,
                 Node* previous_sibling,
                 Node* next_sibling,
                 const std::optional<std::u16string>& attribute_name,
                 const std::optional<std::u16string>& old_value,
                 MutationRecord* record_with_old_value);
  ~MutationRecord() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Type() const;
  Node* Target() const;
  NodeList* AddedNodes();
  NodeList* RemovedNodes();
  Node* PreviousSibling() const;
  Node* NextSibling() const;
  std::optional<std::u16string> AttributeName() const;
  std::optional<std::u16string> AttributeNamespace() const;
  std::optional<std::u16string> OldValue() const;

 private:
  const Kind kind_;
  cppgc::Member<Node> target_;
  // Made empty the first time, for a record of attributes or data.
  cppgc::Member<NodeList> added_nodes_;
  cppgc::Member<NodeList> removed_nodes_;
  cppgc::Member<Node> previous_sibling_;
  cppgc::Member<Node> next_sibling_;
  const std::optional<std::u16string> attribute_name_;
  const std::optional<std::u16string> old_value_;
  // For a record with a null oldValue, the record that it stands for.
  cppgc::Member<MutationRecord> record_with_old_value_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MUTATION_RECORD_H_
