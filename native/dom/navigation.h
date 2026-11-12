// The HTML Standard's Navigation, the window's navigation: the session history
// of a tab that a Session opened, whose one entry is the document. A document
// of an opaque origin, such as about:blank, has no entries and no activation,
// as Blink's Navigation::HasEntriesAndEventsDisabled() says. Navigating,
// traversing and updateCurrentEntry() are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_api.cc

#ifndef MITTO_DOM_NAVIGATION_H_
#define MITTO_DOM_NAVIGATION_H_

#include <vector>

#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class NavigationActivation;
class NavigationHistoryEntry;
class Window;

class Navigation final : public EventTarget {
 public:
  static Navigation* Create(Window* window);
  // One of no window, with no entries, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Navigation(Window* window);
  ~Navigation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  NavigationHistoryEntry* CurrentEntry();
  bindings::ScriptWrappable* Transition() const { return nullptr; }
  NavigationActivation* Activation();
  bool CanGoBack() const { return false; }
  bool CanGoForward() const { return false; }
  std::vector<NavigationHistoryEntry*> Entries();

 private:
  // Makes the entry and the activation the first time a page asks, for a
  // document that has them.
  void MakeEntry();

  cppgc::Member<Window> window_;
  bool made_ = false;
  cppgc::Member<NavigationHistoryEntry> current_entry_;
  cppgc::Member<NavigationActivation> activation_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_H_
