#include "mitto/dom/navigation_history_entry.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_navigation_history_entry.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
NavigationHistoryEntry* NavigationHistoryEntry::Create(
    Window* window,
    std::u16string key,
    std::u16string id,
    std::u16string url,
    uint64_t document_sequence,
    std::optional<SerializedScriptValue> state) {
  return bindings::MakeGarbageCollected<NavigationHistoryEntry>(
      window, std::move(key), std::move(id), std::move(url), document_sequence,
      std::move(state));
}

// static
std::u16string NavigationHistoryEntry::NewKey() {
  return base::UTF8ToUTF16(base::Uuid::GenerateRandomV4().AsLowercaseString());
}

// static
bindings::ScriptWrappable* NavigationHistoryEntry::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<NavigationHistoryEntry>(
      info, bindings::V8NavigationHistoryEntry::kWrapperTypeInfo, nullptr,
      std::u16string(), std::u16string(), std::u16string(), 0, std::nullopt);
}

NavigationHistoryEntry::NavigationHistoryEntry(
    Window* window,
    std::u16string key,
    std::u16string id,
    std::u16string url,
    uint64_t document_sequence,
    std::optional<SerializedScriptValue> state)
    : window_(window),
      key_(std::move(key)),
      id_(std::move(id)),
      url_(std::move(url)),
      document_sequence_(document_sequence),
      state_(std::move(state)) {}

NavigationHistoryEntry::~NavigationHistoryEntry() = default;

const bindings::WrapperTypeInfo* NavigationHistoryEntry::GetWrapperTypeInfo()
    const {
  return &bindings::V8NavigationHistoryEntry::kWrapperTypeInfo;
}

void NavigationHistoryEntry::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
}

bool NavigationHistoryEntry::Attached() const {
  return window_ && window_->HasContext();
}

std::u16string NavigationHistoryEntry::Key() const {
  return Attached() ? key_ : std::u16string();
}

std::u16string NavigationHistoryEntry::Id() const {
  return Attached() ? id_ : std::u16string();
}

std::optional<std::u16string> NavigationHistoryEntry::Url() const {
  if (url_.empty()) {
    return std::nullopt;
  }
  return url_;
}

int64_t NavigationHistoryEntry::Index() const {
  return Attached() ? window_->Navigation()->GetIndexFor(this) : -1;
}

bool NavigationHistoryEntry::SameDocument() const {
  return Attached() &&
         window_->Navigation()->DocumentSequence() == document_sequence_;
}

v8::Local<v8::Value> NavigationHistoryEntry::GetState() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!Attached() || !state_) {
    return v8::Undefined(isolate);
  }
  return state_->Deserialize(isolate);
}

}  // namespace mitto
