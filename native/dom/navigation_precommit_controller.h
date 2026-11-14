// The HTML Standard's NavigationPrecommitController, which an intercepted
// navigation's precommit handlers get: addHandler() adds a handler that runs
// once the navigation has committed. redirect() is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_precommit_controller.h

#ifndef MITTO_DOM_NAVIGATION_PRECOMMIT_CONTROLLER_H_
#define MITTO_DOM_NAVIGATION_PRECOMMIT_CONTROLLER_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class NavigateEvent;

class NavigationPrecommitController final : public bindings::ScriptWrappable {
 public:
  static NavigationPrecommitController* Create(NavigateEvent* event);

  explicit NavigationPrecommitController(NavigateEvent* event);
  ~NavigationPrecommitController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void AddHandler(v8::Local<v8::Function> handler,
                  bindings::ExceptionState& exception_state);

 private:
  cppgc::Member<NavigateEvent> event_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_PRECOMMIT_CONTROLLER_H_
