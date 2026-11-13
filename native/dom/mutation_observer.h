// The DOM's MutationObserver, as Blink's MutationObserver and its helpers
// keep one: each node holds the registrations of the observers that observe
// it, each change to a node's children, attributes or data queues a record
// for each observer that a registration on the node or, for a subtree, on an
// ancestor gives an interest in it, and the observers that have records get
// them in a microtask, in the order the observers were made.
//
// A subtree observer of a node that leaves the tree keeps hearing of changes
// under it until it next gets records, through a transient registration on
// the node that left, as the DOM Standard says.
//
// Upstream: third_party/blink/renderer/core/dom/mutation_observer.h
// Upstream: third_party/blink/renderer/core/dom/mutation_observer.cc
// Upstream: third_party/blink/renderer/core/dom/mutation_observer_registration.cc
// Upstream: third_party/blink/renderer/core/dom/mutation_observer_interest_group.cc
// Upstream: third_party/blink/renderer/core/dom/node.cc

#ifndef MITTO_DOM_MUTATION_OBSERVER_H_
#define MITTO_DOM_MUTATION_OBSERVER_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CharacterData;
class Element;
class MutationObserverRegistration;
class MutationRecord;
class Node;

// What an observer observes, and what a record it gets holds, as Blink's
// MutationObserverOptions: the three types, then the flags.
using MutationObserverOptions = uint8_t;
// The flags of an observer's options that say what its records hold.
using MutationRecordDeliveryOptions = uint8_t;

class MutationObserver final : public bindings::ScriptWrappable {
 public:
  enum : MutationObserverOptions {
    kMutationTypeChildList = 1 << 0,
    kMutationTypeAttributes = 1 << 1,
    kMutationTypeCharacterData = 1 << 2,
    kMutationTypeAll = kMutationTypeChildList | kMutationTypeAttributes |
                       kMutationTypeCharacterData,
    kSubtree = 1 << 3,
    kAttributeFilter = 1 << 4,
    kAttributeOldValue = 1 << 5,
    kCharacterDataOldValue = 1 << 6,
  };

  static MutationObserver* Create(v8::Local<v8::Function> callback);

  explicit MutationObserver(v8::Local<v8::Function> callback);
  ~MutationObserver() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void Observe(Node* target,
               const bindings::MutationObserverInit& options,
               bindings::ExceptionState& exception_state);
  void Disconnect();
  std::vector<MutationRecord*> TakeRecords();

  // What its registrations and the DOM call.
  void ObservationStarted(MutationObserverRegistration* registration);
  void ObservationEnded(MutationObserverRegistration* registration);
  void EnqueueMutationRecord(MutationRecord* record);
  // A transient registration has come, which goes when the observer next gets
  // records, so the observer gets them even if it has none.
  void SetHasTransientRegistration();
  // Hands the observer its records, from the microtask that delivers them.
  void Deliver();
  uint32_t priority() const { return priority_; }

 private:
  v8::TracedReference<v8::Function> callback_;
  HeapVector<MutationRecord> records_;
  // What the last takeRecords() took, until the next.
  HeapVector<MutationRecord> taken_records_;
  HeapVector<MutationObserverRegistration> registrations_;
  // The order of the observers' deliveries: the order they were made in.
  const uint32_t priority_;
};

// An observer's registration on a node, with what it observes there.
class MutationObserverRegistration final
    : public cppgc::GarbageCollected<MutationObserverRegistration> {
 public:
  MutationObserverRegistration(MutationObserver& observer,
                               Node* node,
                               MutationObserverOptions options,
                               std::vector<std::u16string> attribute_filter);
  ~MutationObserverRegistration();

  void Trace(cppgc::Visitor* visitor) const;

  MutationObserver& Observer() const { return *observer_; }
  bool IsSubtree() const { return options_ & MutationObserver::kSubtree; }
  MutationRecordDeliveryOptions DeliveryOptions() const {
    return options_ & (MutationObserver::kAttributeOldValue |
                       MutationObserver::kCharacterDataOldValue);
  }
  // Whether a change of `type` at `node`, of the attribute `attribute_name`
  // for a change of attributes, is one that the registration observes.
  bool ShouldReceiveMutationFrom(const Node& node,
                                 MutationObserverOptions type,
                                 const std::u16string* attribute_name) const;

  // Observes anew, as observe() on the same node does.
  void ResetObservation(MutationObserverOptions options,
                        std::vector<std::u16string> attribute_filter);
  // `node`, under the registration's node, is leaving its tree: a subtree
  // registration follows it with a transient one.
  void ObservedSubtreeNodeWillDetach(Node& node);
  void ClearTransientRegistrations();
  bool HasTransientRegistrations() const { return has_transient_nodes_; }
  // Takes the registration off its node and its observer.
  void Unregister();
  // Ends it, as its node takes it off.
  void Dispose();

 private:
  cppgc::Member<MutationObserver> observer_;
  // Weak, so that an observer does not keep the nodes it observes.
  cppgc::WeakMember<Node> registration_node_;
  // Its node, kept while it has transient registrations.
  cppgc::Member<Node> registration_node_keep_alive_;
  HeapVector<Node> transient_registration_nodes_;
  bool has_transient_nodes_ = false;
  MutationObserverOptions options_;
  std::vector<std::u16string> attribute_filter_;
};

// The registrations that a node holds.
class NodeMutationObserverData final
    : public cppgc::GarbageCollected<NodeMutationObserverData> {
 public:
  NodeMutationObserverData();
  ~NodeMutationObserverData();

  void Trace(cppgc::Visitor* visitor) const;

  HeapVector<MutationObserverRegistration>& registry() { return registry_; }
  HeapVector<MutationObserverRegistration>& transient_registry() {
    return transient_registry_;
  }

 private:
  HeapVector<MutationObserverRegistration> registry_;
  HeapVector<MutationObserverRegistration> transient_registry_;
};

// The observers that a change of one type at one node concerns, with what
// each wants its records to hold.
class MutationObserverInterestGroup final
    : public cppgc::GarbageCollected<MutationObserverInterestGroup> {
 public:
  // Each is null when no observer is concerned.
  static MutationObserverInterestGroup* CreateForChildListMutation(
      Node& target);
  static MutationObserverInterestGroup* CreateForCharacterDataMutation(
      Node& target);
  static MutationObserverInterestGroup* CreateForAttributesMutation(
      Node& target,
      const std::u16string& attribute_name);

  explicit MutationObserverInterestGroup(
      MutationRecordDeliveryOptions old_value_flag);
  ~MutationObserverInterestGroup();

  void Trace(cppgc::Visitor* visitor) const;

  // Queues `record` for each observer: the record itself for one that wants
  // the old value, and a record with a null old value, shared, for the rest.
  void EnqueueMutationRecord(MutationRecord* record);

 private:
  static MutationObserverInterestGroup* CreateIfNeeded(
      Node& target,
      MutationObserverOptions type,
      MutationRecordDeliveryOptions old_value_flag,
      const std::u16string* attribute_name);

  HeapVector<MutationObserver> observers_;
  std::vector<MutationRecordDeliveryOptions> delivery_options_;
  const MutationRecordDeliveryOptions old_value_flag_;
};

// What the DOM's changes call, as Blink's Element::WillModifyAttribute() and
// CharacterData::DidModifyData() do: a record of the attribute `name` of
// `element`, which had `old_value`, or of `node`'s data, which was `old_data`.
void EnqueueAttributeMutation(Element& element,
                              const std::u16string& name,
                              const std::optional<std::u16string>& old_value);
void EnqueueCharacterDataMutation(CharacterData& node,
                                  const std::u16string& old_data);

// Blink's Node::NotifyMutationObserversNodeWillDetach(): `node` is about to
// leave its parent, and the subtree registrations of its ancestors follow it.
void NotifyMutationObserversNodeWillDetach(Node& node);

// Whether any node may have a registration, which spares the DOM the walk up
// the tree for each change while no observer observes, as Blink's
// Document::MayHaveMutationObservers() does.
bool MayHaveMutationObservers();

}  // namespace mitto

#endif  // MITTO_DOM_MUTATION_OBSERVER_H_
