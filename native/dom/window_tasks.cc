#include "mitto/dom/window_tasks.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

WindowTasks::WindowTasks(v8::Isolate* isolate, v8::Local<v8::Context> context)
    : isolate_(isolate), context_(isolate, context) {}

WindowTasks::~WindowTasks() = default;

void WindowTasks::PostTask(TaskType type,
                           base::OnceClosure task,
                           const base::Location& from_here) {
  // The Page program makes the event loop before any window.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(type)->PostTask(
      from_here, base::BindOnce(&WindowTasks::Run, weak_factory_.GetWeakPtr(),
                                type, std::move(task)));
}

void WindowTasks::PostDelayedTask(TaskType type,
                                  base::OnceClosure task,
                                  base::TimeDelta delay,
                                  const base::Location& from_here) {
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(type)->PostDelayedTask(
      from_here,
      base::BindOnce(&WindowTasks::Run, weak_factory_.GetWeakPtr(), type,
                     std::move(task)),
      delay);
}

void WindowTasks::Resume() {
  frozen_ = false;
  for (auto& [type, task] : std::exchange(held_, {})) {
    PostTask(type, std::move(task));
  }
}

void WindowTasks::Run(TaskType type, base::OnceClosure task) {
  if (frozen_) {
    held_.emplace_back(type, std::move(task));
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  std::move(task).Run();
}

}  // namespace mitto
