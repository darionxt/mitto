// Tasks that run in a context, a window's or a worker's, as Blink posts a task
// to the task runner of an execution context: the posted messages of
// postMessage(), MessagePort and BroadcastChannel. A task whose context has
// gone by the time it runs does nothing, as Blink's task runners of a
// destroyed context run nothing.

#ifndef MITTO_DOM_WINDOW_TASK_H_
#define MITTO_DOM_WINDOW_TASK_H_

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

class ExecutionContext;
class Window;

// Runs `task` from a later task of the Page's event loop, of `type`, in
// `window`'s context.
void PostWindowTask(Window* window, TaskType type, base::OnceClosure task);

// Runs `run` now in `window`'s context, and returns whether the window still
// had its context. Nothing holds microtasks back around it: as in Blink, the
// microtasks that each listener of an event queues run once it returns,
// before the next listener, and the rest at the task's end.
bool RunInWindowContext(Window* window, base::FunctionRef<void()> run);

// The same for any context, a window's or a worker's, on its own thread.
bool RunInContext(ExecutionContext* context, base::FunctionRef<void()> run);

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_TASK_H_
