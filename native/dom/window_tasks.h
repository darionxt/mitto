// Tasks of one window, which run in its context on the queue of their task
// type, as Blink posts a task to an ExecutionContext's task runner. They go
// with the window: a task of a window that has gone does not run.

#ifndef MITTO_DOM_WINDOW_TASKS_H_
#define MITTO_DOM_WINDOW_TASKS_H_

#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/event_loop.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class WindowTasks {
 public:
  // The tasks run in `context`.
  WindowTasks(v8::Isolate* isolate, v8::Local<v8::Context> context);

  WindowTasks(const WindowTasks&) = delete;
  WindowTasks& operator=(const WindowTasks&) = delete;

  ~WindowTasks();

  // Runs `task` from a task of `type`, in the window's context, unless the
  // window has gone by then.
  void PostTask(TaskType type,
                base::OnceClosure task,
                const base::Location& from_here = FROM_HERE);
  // The same, once `delay` has passed.
  void PostDelayedTask(TaskType type,
                       base::OnceClosure task,
                       base::TimeDelta delay,
                       const base::Location& from_here = FROM_HERE);

 private:
  void Run(base::OnceClosure task);

  const raw_ptr<v8::Isolate> isolate_;
  const v8::Global<v8::Context> context_;
  base::WeakPtrFactory<WindowTasks> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_TASKS_H_
