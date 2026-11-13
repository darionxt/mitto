// Tasks that run in a window's context, as Blink posts a task to the task
// runner of a window's execution context: the posted messages of
// window.postMessage(), MessagePort and BroadcastChannel. A task whose window
// has lost its context by the time it runs does nothing, as Blink's task
// runners of a destroyed context run nothing.

#ifndef MITTO_DOM_WINDOW_TASK_H_
#define MITTO_DOM_WINDOW_TASK_H_

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"

namespace mitto {

class Window;

// Runs `task` from a later task of the Page's event loop, at the normal
// priority of the HTML Standard's task sources, in `window`'s context. Blink
// posts window.postMessage()'s and MessagePort's tasks on
// TaskType::kPostedMessage, which they move to once the event loop has Blink's
// task types (#270).
void PostWindowTask(Window* window, base::OnceClosure task);

// Runs `run` now in `window`'s context, with a microtask checkpoint after it,
// and returns whether the window still had its context.
bool RunInWindowContext(Window* window, base::FunctionRef<void()> run);

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_TASK_H_
