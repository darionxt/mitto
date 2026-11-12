#include "mitto/dom/navigation.h"

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_navigation.h"
#include "mitto/dom/document.h"
#include "mitto/dom/navigation_activation.h"
#include "mitto/dom/navigation_history_entry.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

// static
Navigation* Navigation::Create(Window* window) {
  return bindings::MakeGarbageCollected<Navigation>(window);
}

// static
bindings::ScriptWrappable* Navigation::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Navigation>(
      info, bindings::V8Navigation::kWrapperTypeInfo, nullptr);
}

Navigation::Navigation(Window* window) : window_(window) {}

Navigation::~Navigation() = default;

const bindings::WrapperTypeInfo* Navigation::GetWrapperTypeInfo() const {
  return &bindings::V8Navigation::kWrapperTypeInfo;
}

void Navigation::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(current_entry_);
  visitor->Trace(activation_);
}

void Navigation::MakeEntry() {
  if (made_) {
    return;
  }
  made_ = true;
  if (!window_) {
    return;
  }
  const std::u16string& url = window_->GetDocument().UrlForBinding();
  if (url::Origin::Create(GURL(url)).opaque()) {
    return;
  }
  current_entry_ = NavigationHistoryEntry::Create(url);
  // A tab's first navigation replaces its initial about:blank.
  activation_ = NavigationActivation::Create(current_entry_, u"replace");
}

NavigationHistoryEntry* Navigation::CurrentEntry() {
  MakeEntry();
  return current_entry_.Get();
}

NavigationActivation* Navigation::Activation() {
  MakeEntry();
  return activation_.Get();
}

std::vector<NavigationHistoryEntry*> Navigation::Entries() {
  MakeEntry();
  if (!current_entry_) {
    return {};
  }
  return {current_entry_.Get()};
}

}  // namespace mitto
