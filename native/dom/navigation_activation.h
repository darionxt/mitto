// The HTML Standard's NavigationActivation, navigation.activation: how the
// document was reached, which for a tab's first document is a navigation that
// replaced the tab's initial about:blank and came from no entry, as Chrome
// gives it, and for one that a document of its origin started, a push, a
// replace or a reload from that document's entry.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_activation.h

#ifndef MITTO_DOM_NAVIGATION_ACTIVATION_H_
#define MITTO_DOM_NAVIGATION_ACTIVATION_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class NavigationHistoryEntry;

class NavigationActivation final : public bindings::ScriptWrappable {
 public:
  static NavigationActivation* Create(NavigationHistoryEntry* entry,
                                      std::u16string navigation_type,
                                      NavigationHistoryEntry* from = nullptr);
  // One with no entry, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NavigationActivation(NavigationHistoryEntry* entry,
                       std::u16string navigation_type,
                       NavigationHistoryEntry* from);
  ~NavigationActivation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  NavigationHistoryEntry* Entry() const { return entry_.Get(); }
  NavigationHistoryEntry* From() const { return from_.Get(); }
  const std::u16string& NavigationType() const { return navigation_type_; }

 private:
  cppgc::Member<NavigationHistoryEntry> entry_;
  cppgc::Member<NavigationHistoryEntry> from_;
  const std::u16string navigation_type_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_ACTIVATION_H_
