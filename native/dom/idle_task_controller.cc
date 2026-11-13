#include "mitto/dom/idle_task_controller.h"

#include <limits>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/idle_deadline.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// WindowIdleTasksManager's kMaxCallbackId.
constexpr uint32_t kMaxCallbackId = std::numeric_limits<int>::max();

EventLoop& EventLoopOfPage() {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  return *event_loop;
}

}  // namespace

struct IdleTaskController::IdleTask {
  IdleTask(IdleTaskController& controller, uint32_t id)
      : controller(controller), id(id) {}

  void OnTimeout() { controller->TimeoutTask(id); }

  const raw_ref<IdleTaskController> controller;
  const uint32_t id;
  v8::Global<v8::Function> callback;
  // Its timeout's task holds a weak pointer, so that the task is cancelled,
  // and counts for no idle period's end, once the callback has run or gone.
  base::WeakPtrFactory<IdleTask> timeout_factory{this};
};

IdleTaskController::IdleTaskController(v8::Isolate* isolate,
                                       v8::Local<v8::Context> context)
    : isolate_(isolate),
      context_(std::make_unique<v8::Global<v8::Context>>(isolate, context)) {}

IdleTaskController::~IdleTaskController() = default;

uint32_t IdleTaskController::NextId() {
  // WindowIdleTasksManager::NextIdleCallbackId(): counts up, from 1 again
  // after the largest int, past those in use.
  while (true) {
    last_id_ = last_id_ == kMaxCallbackId ? 1 : last_id_ + 1;
    if (!tasks_.contains(last_id_)) {
      return last_id_;
    }
  }
}

uint32_t IdleTaskController::RequestIdleCallback(
    v8::Local<v8::Function> callback,
    uint32_t timeout_ms) {
  const uint32_t id = NextId();
  auto task = std::make_unique<IdleTask>(*this, id);
  task->callback.Reset(isolate_, callback);
  // ScriptedIdleTaskController::PostSchedulerIdleAndTimeoutTasks(): the
  // timeout first, as a task of the idle task source, which Blink runs at the
  // normal priority.
  if (timeout_ms > 0) {
    EventLoopOfPage()
        .TaskRunner(EventLoop::Queue::kDefault)
        ->PostDelayedTask(FROM_HERE,
                          base::BindOnce(&IdleTask::OnTimeout,
                                         task->timeout_factory.GetWeakPtr()),
                          base::Milliseconds(timeout_ms));
  }
  tasks_.emplace(id, std::move(task));
  PostSchedulerIdleTask(id);
  return id;
}

void IdleTaskController::CancelIdleCallback(uint32_t id) {
  if (id == 0 || id > kMaxCallbackId) {
    return;
  }
  RemoveIdleTask(id);
}

void IdleTaskController::PostSchedulerIdleTask(uint32_t id) {
  EventLoopOfPage().PostIdleTask(
      FROM_HERE, base::BindOnce(&IdleTaskController::SchedulerIdleTask,
                                weak_factory_.GetWeakPtr(), id));
}

// ScriptedIdleTaskController::SchedulerIdleTask(): no Page is paused, and none
// has the high priority work that would make it yield.
void IdleTaskController::SchedulerIdleTask(uint32_t id,
                                           base::TimeTicks deadline) {
  RunIdleTask(id, deadline, /*did_timeout=*/false);
}

void IdleTaskController::TimeoutTask(uint32_t id) {
  RunIdleTask(id, base::TimeTicks::Now(), /*did_timeout=*/true);
}

// ScriptedIdleTaskController::RunIdleTask(): the callback gets a new
// IdleDeadline, with undefined for `this`, and what it throws is reported as
// an error that nothing caught. The task stays until the callback returns,
// which may cancel it.
void IdleTaskController::RunIdleTask(uint32_t id,
                                     base::TimeTicks deadline,
                                     bool did_timeout) {
  const auto found = tasks_.find(id);
  if (found == tasks_.end()) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_->Get(isolate_);
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::Local<v8::Function> callback = found->second->callback.Get(isolate_);
  // Its timeout goes now, whichever task this is.
  found->second->timeout_factory.InvalidateWeakPtrs();
  const base::WeakPtr<IdleTaskController> self = weak_factory_.GetWeakPtr();
  {
    // WindowIdleTasksManager::V8IdleTask: the callback runs with the
    // background priority current, which a yield() in it inherits.
    const SchedulerTaskScope task_scope(
        isolate_, bindings::MakeGarbageCollected<SchedulerTaskContext>(
                      &Window::Current(), /*abort_source=*/nullptr,
                      SchedulerTaskContext::Priority::kBackground));
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::Value> argument = bindings::ToV8(
        isolate_,
        bindings::MakeGarbageCollected<IdleDeadline>(deadline, did_timeout));
    std::ignore =
        callback->Call(context, v8::Undefined(isolate_), 1, &argument);
    if (try_catch.HasCaught() && try_catch.CanContinue()) {
      bindings::ReportException(isolate_, try_catch.Exception(),
                                try_catch.Message());
    }
  }
  // The callback may have ended the window's context, and this with it.
  if (self) {
    RemoveIdleTask(id);
  }
}

void IdleTaskController::RemoveIdleTask(uint32_t id) {
  tasks_.erase(id);
}

}  // namespace mitto
