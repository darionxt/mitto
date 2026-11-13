#include "mitto/dom/abort_controller.h"

#include "mitto/bindings/v8_abort_controller.h"
#include "mitto/dom/abort_signal.h"

namespace mitto {

// static
AbortController* AbortController::Create() {
  return bindings::MakeGarbageCollected<AbortController>(
      bindings::MakeGarbageCollected<AbortSignal>(
          AbortSignal::SignalType::kController));
}

AbortController::AbortController(AbortSignal* signal) : signal_(signal) {}

AbortController::~AbortController() = default;

const bindings::WrapperTypeInfo* AbortController::GetWrapperTypeInfo() const {
  return &bindings::V8AbortController::kWrapperTypeInfo;
}

void AbortController::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(signal_);
}

void AbortController::Abort(std::optional<v8::Local<v8::Value>> reason) {
  signal_->SignalAbort(reason);
}

}  // namespace mitto
