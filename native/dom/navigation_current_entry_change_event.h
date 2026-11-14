// The HTML Standard's NavigationCurrentEntryChangeEvent, which the window's
// navigation fires once a navigation within the document has changed its
// current entry, with the entry it came from. Its navigationType, a nullable
// enumeration, and its constructor, whose dictionary holds one, are Known Gaps
// until the bindings write a nullable enumeration.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_current_entry_change_event.h

#ifndef MITTO_DOM_NAVIGATION_CURRENT_ENTRY_CHANGE_EVENT_H_
#define MITTO_DOM_NAVIGATION_CURRENT_ENTRY_CHANGE_EVENT_H_

#include <optional>
#include <string>

#include "mitto/dom/event.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class NavigationHistoryEntry;

class NavigationCurrentEntryChangeEvent final : public Event {
 public:
  // A trusted currententrychange, which neither bubbles nor can be canceled.
  static NavigationCurrentEntryChangeEvent* CreateTrusted(
      std::u16string navigation_type,
      NavigationHistoryEntry* from);
  // One from no entry, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NavigationCurrentEntryChangeEvent(
      const std::u16string& type,
      std::optional<std::u16string> navigation_type,
      NavigationHistoryEntry* from);
  ~NavigationCurrentEntryChangeEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  NavigationHistoryEntry* From() const { return from_.Get(); }

 private:
  const std::optional<std::u16string> navigation_type_;
  cppgc::Member<NavigationHistoryEntry> from_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_CURRENT_ENTRY_CHANGE_EVENT_H_
