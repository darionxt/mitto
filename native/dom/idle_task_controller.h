// The idle callbacks of a window's context: requestIdleCallback() and
// cancelIdleCallback() as Chrome runs them. Each is an idle task of the event
// loop, which runs in the next idle period and gets the period's end as its
// deadline. One with a timeout runs as a task of its own once the timeout has
// passed, if it has not run by then.
//
// Upstream: third_party/blink/renderer/core/scheduler/window_idle_tasks_manager.cc
// Upstream: third_party/blink/renderer/core/scheduler/scripted_idle_task_controller.cc

#ifndef MITTO_DOM_IDLE_TASK_CONTROLLER_H_
#define MITTO_DOM_IDLE_TASK_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class IdleTaskController {
 public:
  // The callbacks run in `context`.
  IdleTaskController(v8::Isolate* isolate, v8::Local<v8::Context> context);

  IdleTaskController(const IdleTaskController&) = delete;
  IdleTaskController& operator=(const IdleTaskController&) = delete;

  ~IdleTaskController();

  // Returns the callback's ID, which is never 0.
  uint32_t RequestIdleCallback(v8::Local<v8::Function> callback,
                               uint32_t timeout_ms);
  void CancelIdleCallback(uint32_t id);

  // ScriptedIdleTaskController::ContextDestroyed(): no callback runs again.
  // The one that runs now, which may have ended the context, returns first,
  // and the callbacks go with the controller.
  void Stop();

  // ScriptedIdleTaskController::ContextLifecycleStateChanged(), as the
  // back/forward cache freezes a window: a callback whose idle period or
  // timeout comes while frozen does not run, and Resume() asks for its idle
  // period again and runs its timeout's task, as Blink reschedules them once
  // the context runs again.
  void Freeze() { frozen_ = true; }
  void Resume();

  // How many callbacks wait.
  size_t size() const { return tasks_.size(); }

 private:
  struct IdleTask;

  uint32_t NextId();
  void PostSchedulerIdleTask(uint32_t id);
  void SchedulerIdleTask(uint32_t id, base::TimeTicks deadline);
  void TimeoutTask(uint32_t id);
  void RunIdleTask(uint32_t id, base::TimeTicks deadline, bool did_timeout);
  void RemoveIdleTask(uint32_t id);

  const raw_ptr<v8::Isolate> isolate_;
  const std::unique_ptr<v8::Global<v8::Context>> context_;
  std::map<uint32_t, std::unique_ptr<IdleTask>> tasks_;
  uint32_t last_id_ = 0;
  bool frozen_ = false;
  // The callbacks whose idle periods, or whose timeouts, came while frozen.
  std::vector<uint32_t> held_idle_tasks_;
  std::vector<uint32_t> held_timeouts_;
  base::WeakPtrFactory<IdleTaskController> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_IDLE_TASK_CONTROLLER_H_
