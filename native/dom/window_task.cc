#include "mitto/dom/window_task.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"

namespace mitto {

void PostWindowTask(Window* window, base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(EventLoop::Queue::kDefault)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](cppgc::WeakPersistent<Window> window, base::OnceClosure task) {
                RunInWindowContext(window.Get(),
                                   [&] { std::move(task).Run(); });
              },
              cppgc::WeakPersistent<Window>(window), std::move(task)));
}

bool RunInWindowContext(Window* window, base::FunctionRef<void()> run) {
  if (!window || !window->HasContext()) {
    return false;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  // The window's wrapper is its context's global proxy.
  v8::Local<v8::Context> context;
  if (!window->ToV8(isolate)->GetCreationContext(isolate).ToLocal(&context)) {
    return false;
  }
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  run();
  return true;
}

}  // namespace mitto
