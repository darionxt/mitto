// The idle callbacks of a window's context: requestIdleCallback() and
// cancelIdleCallback() as Chrome runs them. Each waits in the event loop's
// idle queue, which runs only when nothing else is ready, and then runs in an
// idle period that ends 50 milliseconds later or when the next delayed task is
// due, whichever is first, as Blink's long idle periods do for a page that
// draws no frames. One with a timeout runs as a task of its own once the
// timeout has passed, if it has not run by then.
//
// Upstream: third_party/blink/renderer/core/scheduler/window_idle_tasks_manager.cc
// Upstream: third_party/blink/renderer/core/scheduler/scripted_idle_task_controller.cc
// Upstream: third_party/blink/renderer/platform/scheduler/common/idle_helper.cc

#ifndef MITTO_DOM_IDLE_TASK_CONTROLLER_H_
#define MITTO_DOM_IDLE_TASK_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>

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

  // How many callbacks wait.
  size_t size() const { return tasks_.size(); }

 private:
  struct IdleTask;

  uint32_t NextId();
  // A task of the idle queue for `id`: it starts an idle period and runs the
  // callback in it, or tries again soon when the next delayed task is too
  // near for one.
  void PostSchedulerIdleTask(uint32_t id, base::TimeDelta delay);
  void SchedulerIdleTask(uint32_t id);
  void TimeoutTask(uint32_t id);
  void RunIdleTask(uint32_t id, base::TimeTicks deadline, bool did_timeout);
  void RemoveIdleTask(uint32_t id);

  const raw_ptr<v8::Isolate> isolate_;
  const std::unique_ptr<v8::Global<v8::Context>> context_;
  std::map<uint32_t, std::unique_ptr<IdleTask>> tasks_;
  uint32_t last_id_ = 0;
  base::WeakPtrFactory<IdleTaskController> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_IDLE_TASK_CONTROLLER_H_
