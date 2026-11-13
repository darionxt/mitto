#include "mitto/dom/mutation_observer.h"

#include <algorithm>
#include <utility>

#include "base/no_destructor.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_mutation_observer.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/element.h"
#include "mitto/dom/mutation_record.h"
#include "mitto/dom/node.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"

namespace mitto {

namespace {

// Blink's g_observer_priority.
uint32_t g_observer_priority = 0;

// How many registrations a node may still hold, as Blink's document counts
// the types of its observers. It only grows by registrations that nodes
// still hold or held when they were collected, so it is never too small.
size_t g_registrations = 0;

// Blink's MutationObserverAgentData: the observers that have records or
// transient registrations, which the next delivery hands them, and whether
// that delivery is queued, which it is while any wait.
struct Delivery {
  std::vector<cppgc::Persistent<MutationObserver>> active_observers;
};

Delivery& GetDelivery() {
  static base::NoDestructor<Delivery> delivery;
  return *delivery;
}

// The DOM Standard's notify mutation observers, from its microtask.
void DeliverMutations(void* data) {
  std::vector<cppgc::Persistent<MutationObserver>> observers;
  observers.swap(GetDelivery().active_observers);
  std::sort(observers.begin(), observers.end(),
            [](const cppgc::Persistent<MutationObserver>& a,
               const cppgc::Persistent<MutationObserver>& b) {
              return a->priority() < b->priority();
            });
  for (const cppgc::Persistent<MutationObserver>& observer : observers) {
    observer->Deliver();
  }
}

// Blink's ActivateObserver(): the observer gets what it has in the next
// delivery, which the first observer to wait queues, as a microtask.
void ActivateObserver(MutationObserver* observer) {
  std::vector<cppgc::Persistent<MutationObserver>>& active =
      GetDelivery().active_observers;
  if (active.empty()) {
    v8::Isolate::GetCurrent()->EnqueueMicrotask(&DeliverMutations, nullptr);
  }
  for (const cppgc::Persistent<MutationObserver>& waiting : active) {
    if (waiting.Get() == observer) {
      return;
    }
  }
  active.emplace_back(observer);
}

// Adds the registrations of `registry` that observe a change of `type` at
// `target` to `observers`, with the delivery options of all of an
// observer's, as Blink's CollectMatchingObserversForMutation() does.
void CollectMatchingObservers(
    HeapVector<MutationObserverRegistration>& registry,
    const Node& target,
    MutationObserverOptions type,
    const std::u16string* attribute_name,
    std::vector<std::pair<MutationObserver*, MutationRecordDeliveryOptions>>&
        observers) {
  for (size_t i = 0; i < registry.size(); ++i) {
    MutationObserverRegistration* const registration = registry[i];
    if (!registration->ShouldReceiveMutationFrom(target, type,
                                                 attribute_name)) {
      continue;
    }
    MutationObserver* const observer = &registration->Observer();
    auto found = std::find_if(
        observers.begin(), observers.end(),
        [observer](const auto& item) { return item.first == observer; });
    if (found == observers.end()) {
      observers.emplace_back(observer, registration->DeliveryOptions());
    } else {
      found->second |= registration->DeliveryOptions();
    }
  }
}

}  // namespace

// static
MutationObserver* MutationObserver::Create(v8::Local<v8::Function> callback) {
  return bindings::MakeGarbageCollected<MutationObserver>(callback);
}

MutationObserver::MutationObserver(v8::Local<v8::Function> callback)
    : callback_(v8::Isolate::GetCurrent(), callback),
      priority_(g_observer_priority++) {}

MutationObserver::~MutationObserver() = default;

const bindings::WrapperTypeInfo* MutationObserver::GetWrapperTypeInfo() const {
  return &bindings::V8MutationObserver::kWrapperTypeInfo;
}

void MutationObserver::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(callback_);
  records_.Trace(visitor);
  taken_records_.Trace(visitor);
  registrations_.Trace(visitor);
}

void MutationObserver::Observe(Node* target,
                               const bindings::MutationObserverInit& init,
                               bindings::ExceptionState& exception_state) {
  // Blink's MutationObserver::observe(), with its messages.
  MutationObserverOptions options = 0;
  if (init.attribute_old_value.value_or(false)) {
    options |= kAttributeOldValue;
  }
  std::vector<std::u16string> attribute_filter;
  if (init.attribute_filter) {
    attribute_filter = *init.attribute_filter;
    options |= kAttributeFilter;
  }
  const bool attributes = init.attributes.value_or(false);
  if (attributes || (!init.attributes &&
                     (init.attribute_old_value || init.attribute_filter))) {
    options |= kMutationTypeAttributes;
  }
  if (init.character_data_old_value.value_or(false)) {
    options |= kCharacterDataOldValue;
  }
  const bool character_data = init.character_data.value_or(false);
  if (character_data ||
      (!init.character_data && init.character_data_old_value)) {
    options |= kMutationTypeCharacterData;
  }
  if (init.child_list) {
    options |= kMutationTypeChildList;
  }
  if (init.subtree) {
    options |= kSubtree;
  }
  if (!(options & kMutationTypeAttributes)) {
    if (options & kAttributeOldValue) {
      exception_state.ThrowTypeError(
          "The options object may only set 'attributeOldValue' to true when "
          "'attributes' is true or not present.");
      return;
    }
    if (options & kAttributeFilter) {
      exception_state.ThrowTypeError(
          "The options object may only set 'attributeFilter' when "
          "'attributes' is true or not present.");
      return;
    }
  }
  if ((options & kCharacterDataOldValue) &&
      !(options & kMutationTypeCharacterData)) {
    exception_state.ThrowTypeError(
        "The options object may only set 'characterDataOldValue' to true when "
        "'characterData' is true or not present.");
    return;
  }
  if (!(options & kMutationTypeAll)) {
    exception_state.ThrowTypeError(
        "The options object must set at least one of 'attributes', "
        "'characterData', or 'childList' to true.");
    return;
  }
  // Blink's Node::RegisterMutationObserver(): the observer's registration on
  // the node observes anew, or a new one comes.
  NodeMutationObserverData& data = target->EnsureMutationObserverData();
  HeapVector<MutationObserverRegistration>& registry = data.registry();
  for (size_t i = 0; i < registry.size(); ++i) {
    if (&registry[i]->Observer() == this) {
      registry[i]->ResetObservation(options, std::move(attribute_filter));
      return;
    }
  }
  registry.push_back(
      bindings::MakeGarbageCollected<MutationObserverRegistration>(
          *this, target, options, std::move(attribute_filter)));
  ++g_registrations;
}

void MutationObserver::Disconnect() {
  records_.clear();
  // A registration may go while the others are taken off.
  const std::vector<MutationObserverRegistration*> registrations =
      registrations_.ToVector();
  for (MutationObserverRegistration* registration : registrations) {
    bool still_there = false;
    for (size_t i = 0; i < registrations_.size(); ++i) {
      still_there |= registrations_[i] == registration;
    }
    if (still_there) {
      registration->Unregister();
    }
  }
}

std::vector<MutationRecord*> MutationObserver::TakeRecords() {
  // The bindings make the records' wrappers once this returns, which can run
  // the collector, so the records stay traced until the next take.
  taken_records_.clear();
  for (MutationRecord* record : records_.ToVector()) {
    taken_records_.push_back(record);
  }
  records_.clear();
  return taken_records_.ToVector();
}

void MutationObserver::ObservationStarted(
    MutationObserverRegistration* registration) {
  registrations_.push_back(registration);
}

void MutationObserver::ObservationEnded(
    MutationObserverRegistration* registration) {
  registrations_.EraseIf([registration](MutationObserverRegistration* item) {
    return item == registration;
  });
}

void MutationObserver::EnqueueMutationRecord(MutationRecord* record) {
  records_.push_back(record);
  ActivateObserver(this);
}

void MutationObserver::SetHasTransientRegistration() {
  ActivateObserver(this);
}

void MutationObserver::Deliver() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  // The callback's realm, whose window must still run its scripts, as
  // Blink's observer needs its execution context.
  const v8::Local<v8::Function> callback = callback_.Get(isolate);
  v8::Local<v8::Context> context;
  if (!callback->GetCreationContext(isolate).ToLocal(&context)) {
    return;
  }
  const v8::Context::Scope context_scope(context);
  Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    return;
  }
  // Clearing a transient registration can change the registrations, so the
  // ones that have any are taken first.
  std::vector<MutationObserverRegistration*> transient;
  for (size_t i = 0; i < registrations_.size(); ++i) {
    if (registrations_[i]->HasTransientRegistrations()) {
      transient.push_back(registrations_[i]);
    }
  }
  for (MutationObserverRegistration* registration : transient) {
    registration->ClearTransientRegistrations();
  }
  if (records_.empty()) {
    return;
  }
  // Held while their wrappers are made, which can run the collector, since
  // nothing else holds them once they are taken.
  std::vector<cppgc::Persistent<MutationRecord>> records;
  records.reserve(records_.size());
  for (MutationRecord* record : records_.ToVector()) {
    records.emplace_back(record);
  }
  records_.clear();
  // The DOM Standard: the callback with the records and the observer, and
  // the observer as `this`; what it throws is reported.
  std::vector<v8::Local<v8::Value>> wrappers;
  wrappers.reserve(records.size());
  for (const cppgc::Persistent<MutationRecord>& record : records) {
    wrappers.push_back(record->ToV8(isolate));
  }
  const v8::Local<v8::Value> observer = ToV8(isolate);
  v8::Local<v8::Value> arguments[] = {
      v8::Array::New(isolate, wrappers.data(), wrappers.size()), observer};
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  if (callback->Call(context, observer, 2, arguments).IsEmpty() &&
      try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
}

MutationObserverRegistration::MutationObserverRegistration(
    MutationObserver& observer,
    Node* node,
    MutationObserverOptions options,
    std::vector<std::u16string> attribute_filter)
    : observer_(&observer),
      registration_node_(node),
      options_(options),
      attribute_filter_(std::move(attribute_filter)) {
  observer_->ObservationStarted(this);
}

MutationObserverRegistration::~MutationObserverRegistration() = default;

void MutationObserverRegistration::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(observer_);
  visitor->Trace(registration_node_);
  visitor->Trace(registration_node_keep_alive_);
  transient_registration_nodes_.Trace(visitor);
}

bool MutationObserverRegistration::ShouldReceiveMutationFrom(
    const Node& node,
    MutationObserverOptions type,
    const std::u16string* attribute_name) const {
  if (!(options_ & type)) {
    return false;
  }
  if (registration_node_.Get() != &node && !IsSubtree()) {
    return false;
  }
  if (type != MutationObserver::kMutationTypeAttributes ||
      !(options_ & MutationObserver::kAttributeFilter)) {
    return true;
  }
  return std::find(attribute_filter_.begin(), attribute_filter_.end(),
                   *attribute_name) != attribute_filter_.end();
}

void MutationObserverRegistration::ResetObservation(
    MutationObserverOptions options,
    std::vector<std::u16string> attribute_filter) {
  ClearTransientRegistrations();
  options_ = options;
  attribute_filter_ = std::move(attribute_filter);
}

void MutationObserverRegistration::ObservedSubtreeNodeWillDetach(Node& node) {
  if (!IsSubtree()) {
    return;
  }
  node.EnsureMutationObserverData().transient_registry().push_back(this);
  ++g_registrations;
  observer_->SetHasTransientRegistration();
  if (!has_transient_nodes_) {
    has_transient_nodes_ = true;
    registration_node_keep_alive_ = registration_node_.Get();
  }
  transient_registration_nodes_.push_back(&node);
}

void MutationObserverRegistration::ClearTransientRegistrations() {
  if (!has_transient_nodes_) {
    return;
  }
  for (size_t i = 0; i < transient_registration_nodes_.size(); ++i) {
    if (NodeMutationObserverData* const data =
            transient_registration_nodes_[i]->MutationObserverData()) {
      data->transient_registry().EraseIf(
          [this](MutationObserverRegistration* item) { return item == this; });
    }
  }
  transient_registration_nodes_.clear();
  has_transient_nodes_ = false;
  registration_node_keep_alive_ = nullptr;
}

void MutationObserverRegistration::Unregister() {
  // Blink's Node::UnregisterMutationObserver(), for a node that may have
  // gone.
  Node* const node = registration_node_.Get();
  Dispose();
  if (node) {
    if (NodeMutationObserverData* const data = node->MutationObserverData()) {
      data->registry().EraseIf(
          [this](MutationObserverRegistration* item) { return item == this; });
    }
  }
}

void MutationObserverRegistration::Dispose() {
  ClearTransientRegistrations();
  if (observer_) {
    observer_->ObservationEnded(this);
    observer_ = nullptr;
  }
}

NodeMutationObserverData::NodeMutationObserverData() = default;
NodeMutationObserverData::~NodeMutationObserverData() = default;

void NodeMutationObserverData::Trace(cppgc::Visitor* visitor) const {
  registry_.Trace(visitor);
  transient_registry_.Trace(visitor);
}

// static
MutationObserverInterestGroup*
MutationObserverInterestGroup::CreateForChildListMutation(Node& target) {
  return CreateIfNeeded(target, MutationObserver::kMutationTypeChildList, 0,
                        nullptr);
}

// static
MutationObserverInterestGroup*
MutationObserverInterestGroup::CreateForCharacterDataMutation(Node& target) {
  return CreateIfNeeded(target, MutationObserver::kMutationTypeCharacterData,
                        MutationObserver::kCharacterDataOldValue, nullptr);
}

// static
MutationObserverInterestGroup*
MutationObserverInterestGroup::CreateForAttributesMutation(
    Node& target,
    const std::u16string& attribute_name) {
  return CreateIfNeeded(target, MutationObserver::kMutationTypeAttributes,
                        MutationObserver::kAttributeOldValue, &attribute_name);
}

// static
MutationObserverInterestGroup* MutationObserverInterestGroup::CreateIfNeeded(
    Node& target,
    MutationObserverOptions type,
    MutationRecordDeliveryOptions old_value_flag,
    const std::u16string* attribute_name) {
  if (!MayHaveMutationObservers()) {
    return nullptr;
  }
  // Blink's Node::GetRegisteredMutationObserversOfType(): the node's own
  // registrations, then each ancestor's.
  std::vector<std::pair<MutationObserver*, MutationRecordDeliveryOptions>>
      observers;
  for (Node* node = &target; node; node = node->ParentNode()) {
    if (NodeMutationObserverData* const data = node->MutationObserverData()) {
      CollectMatchingObservers(data->registry(), target, type, attribute_name,
                               observers);
      CollectMatchingObservers(data->transient_registry(), target, type,
                               attribute_name, observers);
    }
  }
  if (observers.empty()) {
    return nullptr;
  }
  auto* const group =
      bindings::MakeGarbageCollected<MutationObserverInterestGroup>(
          old_value_flag);
  for (const auto& [observer, options] : observers) {
    group->observers_.push_back(observer);
    group->delivery_options_.push_back(options);
  }
  return group;
}

MutationObserverInterestGroup::MutationObserverInterestGroup(
    MutationRecordDeliveryOptions old_value_flag)
    : old_value_flag_(old_value_flag) {}

MutationObserverInterestGroup::~MutationObserverInterestGroup() = default;

void MutationObserverInterestGroup::Trace(cppgc::Visitor* visitor) const {
  observers_.Trace(visitor);
}

void MutationObserverInterestGroup::EnqueueMutationRecord(
    MutationRecord* record) {
  const cppgc::Persistent<MutationRecord> held(record);
  cppgc::Persistent<MutationRecord> with_null_old_value;
  for (size_t i = 0; i < observers_.size(); ++i) {
    MutationObserver* const observer = observers_[i];
    if (delivery_options_[i] & old_value_flag_) {
      observer->EnqueueMutationRecord(record);
      continue;
    }
    if (!with_null_old_value) {
      with_null_old_value = record->OldValue()
                                ? MutationRecord::CreateWithNullOldValue(record)
                                : record;
    }
    observer->EnqueueMutationRecord(with_null_old_value.Get());
  }
}

void EnqueueAttributeMutation(Element& element,
                              const std::u16string& name,
                              const std::optional<std::u16string>& old_value) {
  if (MutationObserverInterestGroup* const recipients =
          MutationObserverInterestGroup::CreateForAttributesMutation(element,
                                                                     name)) {
    const cppgc::Persistent<MutationObserverInterestGroup> held(recipients);
    recipients->EnqueueMutationRecord(
        MutationRecord::CreateAttributes(&element, name, old_value));
  }
}

void EnqueueCharacterDataMutation(CharacterData& node,
                                  const std::u16string& old_data) {
  if (MutationObserverInterestGroup* const recipients =
          MutationObserverInterestGroup::CreateForCharacterDataMutation(node)) {
    const cppgc::Persistent<MutationObserverInterestGroup> held(recipients);
    recipients->EnqueueMutationRecord(
        MutationRecord::CreateCharacterData(&node, old_data));
  }
}

void NotifyMutationObserversNodeWillDetach(Node& node) {
  if (!MayHaveMutationObservers()) {
    return;
  }
  for (Node* ancestor = node.ParentNode(); ancestor;
       ancestor = ancestor->ParentNode()) {
    NodeMutationObserverData* const data = ancestor->MutationObserverData();
    if (!data) {
      continue;
    }
    // Copies, since following the node adds to the node's own lists.
    for (MutationObserverRegistration* registration :
         data->registry().ToVector()) {
      registration->ObservedSubtreeNodeWillDetach(node);
    }
    for (MutationObserverRegistration* registration :
         data->transient_registry().ToVector()) {
      registration->ObservedSubtreeNodeWillDetach(node);
    }
  }
}

bool MayHaveMutationObservers() {
  return g_registrations != 0;
}

}  // namespace mitto
