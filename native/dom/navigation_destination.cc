#include "mitto/dom/navigation_destination.h"

#include <utility>

#include "mitto/bindings/v8_navigation_destination.h"
#include "mitto/dom/navigation_history_entry.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
NavigationDestination* NavigationDestination::Create(
    std::u16string url,
    bool same_document,
    std::optional<SerializedScriptValue> state,
    NavigationHistoryEntry* entry) {
  return bindings::MakeGarbageCollected<NavigationDestination>(
      std::move(url), same_document, std::move(state), entry);
}

NavigationDestination::NavigationDestination(
    std::u16string url,
    bool same_document,
    std::optional<SerializedScriptValue> state,
    NavigationHistoryEntry* entry)
    : url_(std::move(url)),
      same_document_(same_document),
      state_(std::move(state)),
      entry_(entry) {}

NavigationDestination::~NavigationDestination() = default;

const bindings::WrapperTypeInfo* NavigationDestination::GetWrapperTypeInfo()
    const {
  return &bindings::V8NavigationDestination::kWrapperTypeInfo;
}

void NavigationDestination::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(entry_);
}

std::u16string NavigationDestination::Key() const {
  return entry_ ? entry_->key() : std::u16string();
}

std::u16string NavigationDestination::Id() const {
  return entry_ ? entry_->id() : std::u16string();
}

int64_t NavigationDestination::Index() const {
  return entry_ ? entry_->Index() : -1;
}

v8::Local<v8::Value> NavigationDestination::GetState() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!state_) {
    return v8::Undefined(isolate);
  }
  return state_->Deserialize(isolate);
}

}  // namespace mitto
