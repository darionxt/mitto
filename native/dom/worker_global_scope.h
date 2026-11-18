// The HTML Standard's WorkerGlobalScope, the global object of a worker's
// context, with `self` and what every ExecutionContext has, the timers,
// queueMicrotask(), atob(), btoa() and reportError(); its other members are
// Known Gaps.
//
// As a window, a worker's context has its global object and its global proxy
// both wrap it, so that a call whose receiver is either finds it.
//
// Upstream: third_party/blink/renderer/core/workers/worker_global_scope.h
// Upstream: third_party/blink/renderer/core/workers/worker_or_worklet_global_scope.h
// Upstream: third_party/blink/renderer/bindings/core/v8/worker_or_worklet_script_controller.cc

#ifndef MITTO_DOM_WORKER_GLOBAL_SCOPE_H_
#define MITTO_DOM_WORKER_GLOBAL_SCOPE_H_

#include "mitto/dom/event_target.h"
#include "mitto/dom/execution_context.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class WorkerGlobalScope : public EventTarget, public ExecutionContext {
 public:
  ~WorkerGlobalScope() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  WorkerGlobalScope* Self() { return this; }

  // What the Page uses.
  // Makes the global proxy and the global object of `context`, which was made
  // from the template of this scope's interface, this scope's wrappers, as
  // Blink's WorkerOrWorkletScriptController::Initialize() does.
  void AssociateWithContext(v8::Isolate* isolate,
                            v8::Local<v8::Context> context);

 protected:
  WorkerGlobalScope();
};

}  // namespace mitto

#endif  // MITTO_DOM_WORKER_GLOBAL_SCOPE_H_
