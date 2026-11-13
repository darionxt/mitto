#include "mitto/dom/task_controller.h"

#include "mitto/bindings/v8_task_controller.h"
#include "mitto/dom/task_signal.h"

namespace mitto {

// static
TaskController* TaskController::Create(
    const bindings::TaskControllerInit& init) {
  return bindings::MakeGarbageCollected<TaskController>(
      bindings::MakeGarbageCollected<TaskSignal>(
          init.priority, AbortSignal::SignalType::kController));
}

TaskController::TaskController(AbortSignal* signal) : AbortController(signal) {}

TaskController::~TaskController() = default;

const bindings::WrapperTypeInfo* TaskController::GetWrapperTypeInfo() const {
  return &bindings::V8TaskController::kWrapperTypeInfo;
}

}  // namespace mitto
