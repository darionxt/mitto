// window.scheduler: Prioritized Task Scheduling as Blink's DOMScheduler runs
// it. postTask() posts its callback, after its delay, to the event loop's queue
// for its priority: the option's, its TaskSignal's, or user-visible. The
// promise settles as the callback returns or throws, and rejects with the
// signal's reason if the signal aborts before the callback has returned.
// yield() posts a continuation to the queue that comes before the tasks of the
// priority it inherits from the current task, and with it the signal, as
// SchedulerTaskContext says.
//
// A TaskSignal keeps the priority that it was made with, since setPriority()
// is a Known Gap, so its tasks share the queue of that priority, which orders
// them as Blink's queue of their own would.
//
// Upstream: third_party/blink/renderer/core/scheduler/dom_scheduler.cc
// Upstream: third_party/blink/renderer/core/scheduler/dom_task.cc
// Upstream: third_party/blink/renderer/core/scheduler/dom_task_continuation.cc

#ifndef MITTO_DOM_SCHEDULER_H_
#define MITTO_DOM_SCHEDULER_H_

#include <stddef.h>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Window;

class Scheduler final : public bindings::ScriptWrappable {
 public:
  // The scheduler of `window`.
  explicit Scheduler(Window* window);
  ~Scheduler() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> PostTask(
      v8::Local<v8::Function> callback,
      const bindings::SchedulerPostTaskOptions& options,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Yield(bindings::ExceptionState& exception_state);

  // How many postTask() callbacks, delayed ones among them, and yield()
  // continuations wait to run, but for those that their signal aborted.
  size_t pending_tasks() const { return pending_tasks_; }
  // A task or a continuation no longer waits: it runs, or its signal aborted
  // it.
  void PendingTaskEnded() { --pending_tasks_; }

 private:
  // Whether the window still runs its context, or throws if not.
  bool CheckAttached(bindings::ExceptionState& exception_state) const;

  const cppgc::WeakMember<Window> window_;
  size_t pending_tasks_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCHEDULER_H_
