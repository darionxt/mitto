// Prioritized Task Scheduling's TaskController, as Blink's DOMTaskController:
// an AbortController whose signal is a TaskSignal of the priority it is made
// with. setPriority() is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/scheduler/dom_task_controller.h
// Upstream: third_party/blink/renderer/core/scheduler/dom_task_controller.cc

#ifndef MITTO_DOM_TASK_CONTROLLER_H_
#define MITTO_DOM_TASK_CONTROLLER_H_

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/abort_controller.h"

namespace mitto {

class TaskController final : public AbortController {
 public:
  static TaskController* Create(const bindings::TaskControllerInit& init);

  explicit TaskController(AbortSignal* signal);
  ~TaskController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_TASK_CONTROLLER_H_
