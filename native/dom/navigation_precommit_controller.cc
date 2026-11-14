#include "mitto/dom/navigation_precommit_controller.h"

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_navigation_precommit_controller.h"
#include "mitto/dom/navigate_event.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// static
NavigationPrecommitController* NavigationPrecommitController::Create(
    NavigateEvent* event) {
  return bindings::MakeGarbageCollected<NavigationPrecommitController>(event);
}

NavigationPrecommitController::NavigationPrecommitController(
    NavigateEvent* event)
    : event_(event) {}

NavigationPrecommitController::~NavigationPrecommitController() = default;

const bindings::WrapperTypeInfo*
NavigationPrecommitController::GetWrapperTypeInfo() const {
  return &bindings::V8NavigationPrecommitController::kWrapperTypeInfo;
}

void NavigationPrecommitController::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(event_);
}

void NavigationPrecommitController::AddHandler(
    v8::Local<v8::Function> handler,
    bindings::ExceptionState& exception_state) {
  event_->AddHandler(handler, exception_state);
}

}  // namespace mitto
