// The DOM's AbortController, as Blink's: it holds a signal, which abort()
// aborts.
//
// Blink's controller settles its signal when the collector takes it, so that
// signals that follow it stop following a signal that can no longer abort.
// Here those links are weak both ways (abort_signal.h), and a controller that
// goes takes nothing else with it.
//
// Upstream: third_party/blink/renderer/core/dom/abort_controller.h
// Upstream: third_party/blink/renderer/core/dom/abort_controller.cc

#ifndef MITTO_DOM_ABORT_CONTROLLER_H_
#define MITTO_DOM_ABORT_CONTROLLER_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class AbortSignal;

class AbortController : public bindings::ScriptWrappable {
 public:
  static AbortController* Create();

  explicit AbortController(AbortSignal* signal);
  ~AbortController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  AbortSignal* Signal() const { return signal_.Get(); }
  void Abort(std::optional<v8::Local<v8::Value>> reason);

 private:
  const cppgc::Member<AbortSignal> signal_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ABORT_CONTROLLER_H_
