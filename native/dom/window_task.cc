#include "mitto/dom/window_task.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"

namespace mitto {
namespace {

// Runs `task` in `context`, or, for a window that the back/forward cache keeps,
// holds it until the window comes back, as Blink's frozen frames hold their
// tasks: a `delayed` one, whose delay came due meanwhile, after what the
// window's resume and pageshow listeners post.
void RunOrHoldInContext(cppgc::WeakPersistent<ExecutionContext> context,
                        TaskType type,
                        bool delayed,
                        base::OnceClosure task) {
  if (context && context->IsInBackForwardCache()) {
    context->HoldTask(type, std::move(task), delayed);
    return;
  }
  RunInContext(context.Get(), [&] { std::move(task).Run(); });
}

}  // namespace

void PostWindowTask(Window* window, TaskType type, base::OnceClosure task) {
  PostTaskInContext(window, type, std::move(task));
}

void PostTaskInContext(ExecutionContext* context,
                       TaskType type,
                       base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(type)->PostTask(
      FROM_HERE,
      base::BindOnce(&RunOrHoldInContext,
                     cppgc::WeakPersistent<ExecutionContext>(context), type,
                     /*delayed=*/false, std::move(task)));
}

ExecutionContext* FrozenContext(v8::Isolate* isolate,
                                v8::Local<v8::Context> context) {
  ExecutionContext* const execution_context =
      ExecutionContext::From(isolate, context);
  return execution_context && execution_context->IsInBackForwardCache()
             ? execution_context
             : nullptr;
}

void PostDelayedTaskInContext(ExecutionContext* context,
                              TaskType type,
                              base::OnceClosure task,
                              base::TimeDelta delay) {
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(type)->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RunOrHoldInContext,
                     cppgc::WeakPersistent<ExecutionContext>(context), type,
                     /*delayed=*/delay.is_positive(), std::move(task)),
      delay);
}

bool RunInWindowContext(Window* window, base::FunctionRef<void()> run) {
  return RunInContext(window, run);
}

bool RunInContext(ExecutionContext* execution_context,
                  base::FunctionRef<void()> run) {
  if (!execution_context || !execution_context->HasContext()) {
    return false;
  }
  // Something would run in a window that the back/forward cache keeps
  // frozen, whose page Chrome evicts for a script, as LocalFrame's
  // BackForwardCacheEviction hook does, or for a message of a
  // BroadcastChannel, as BroadcastChannel::OnMessage() does.
  if (execution_context->IsInBackForwardCache()) {
    if (execution_context->IsWindow()) {
      static_cast<Window*>(execution_context)->EvictFromBackForwardCache();
    }
    return false;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  // The wrapper of a window or a worker's global scope is its context's
  // global proxy.
  v8::Local<v8::Context> context;
  if (!execution_context->ErrorEventTarget()
           ->ToV8(isolate)
           ->GetCreationContext(isolate)
           .ToLocal(&context)) {
    return false;
  }
  const v8::Context::Scope context_scope(context);
  // Holds back no microtasks: a listener's own scope is the outermost one
  // that runs them, as V8ScriptRunner::CallFunction()'s is in Blink, and the
  // event loop runs the rest at the task's end.
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  run();
  return true;
}

}  // namespace mitto
