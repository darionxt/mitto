// The HTML Standard's NavigationTransition, navigation.transition while an
// intercepted navigation goes on: its type, the entry it comes from, its
// destination, and promises of its commit and its end. The end's is marked as
// handled, as Blink marks it.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_transition.cc

#ifndef MITTO_DOM_NAVIGATION_TRANSITION_H_
#define MITTO_DOM_NAVIGATION_TRANSITION_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class NavigationDestination;
class NavigationHistoryEntry;

class NavigationTransition final : public bindings::ScriptWrappable {
 public:
  // Made in the current context, whose promises it holds.
  static NavigationTransition* Create(std::u16string navigation_type,
                                      NavigationHistoryEntry* from,
                                      NavigationDestination* to);

  NavigationTransition(std::u16string navigation_type,
                       NavigationHistoryEntry* from,
                       NavigationDestination* to);
  ~NavigationTransition() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& NavigationType() const { return navigation_type_; }
  NavigationHistoryEntry* From() const { return from_.Get(); }
  NavigationDestination* To() const { return to_.Get(); }
  v8::Local<v8::Promise> Committed() const;
  v8::Local<v8::Promise> Finished() const;

  // What the window's Navigation settles.
  void ResolveCommitted();
  void ResolveFinished();
  // Rejects the end, and the commit while it waits, with `reason`.
  void RejectFinished(v8::Local<v8::Value> reason);

 private:
  const std::u16string navigation_type_;
  cppgc::Member<NavigationHistoryEntry> from_;
  cppgc::Member<NavigationDestination> to_;
  v8::TracedReference<v8::Promise::Resolver> committed_;
  v8::TracedReference<v8::Promise::Resolver> finished_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_TRANSITION_H_
