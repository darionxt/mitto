// The state that scheduler.yield() inherits, as Blink's task attribution
// carries it: a scheduler.postTask() callback runs with its signal and
// priority, and an idle callback with the background priority, and V8 hands
// that state on to the promise reactions they leave, through the isolate's
// continuation-preserved embedder data. A timer, an event or a script outside
// those carries none, so a yield() there continues at the user-visible
// priority.
//
// Upstream: third_party/blink/renderer/core/scheduler/scheduler_task_context.h
// Upstream: third_party/blink/renderer/core/scheduler/web_scheduling_task_state.cc
// Upstream: third_party/blink/renderer/core/scheduler/task_attribution_task_state.cc
// Upstream: third_party/blink/renderer/core/scheduler/task_attribution_tracker_impl.cc

#ifndef MITTO_DOM_SCHEDULER_TASK_CONTEXT_H_
#define MITTO_DOM_SCHEDULER_TASK_CONTEXT_H_

#include "base/memory/raw_ptr.h"
#include "mitto/dom/event_loop.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class AbortSignal;
class Window;

class SchedulerTaskContext final
    : public cppgc::GarbageCollected<SchedulerTaskContext> {
 public:
  // TaskPriority's values.
  enum class Priority { kUserBlocking, kUserVisible, kBackground };

  // The state of the current task or reaction, or null for none.
  static SchedulerTaskContext* Current(v8::Isolate* isolate);
  // The event loop's queue for `priority`, for a task or for a continuation of
  // yield().
  static EventLoop::Queue QueueFor(Priority priority, bool continuation);

  SchedulerTaskContext(Window* window,
                       AbortSignal* abort_source,
                       Priority priority);

  void Trace(cppgc::Visitor* visitor) const;

  // Whether a yield() of `window`'s scheduler inherits this, as
  // SchedulerTaskContext::CanPropagateTo() says: only within its own window.
  bool CanPropagateTo(const Window* window) const;
  AbortSignal* abort_source() const { return abort_source_.Get(); }
  Priority priority() const { return priority_; }

 private:
  const cppgc::WeakMember<Window> window_;
  const cppgc::Member<AbortSignal> abort_source_;
  const Priority priority_;
};

// Makes `context` the current state while it lives, and the one before it
// again after, as TaskAttributionTracker::TaskScope does.
class SchedulerTaskScope final {
 public:
  SchedulerTaskScope(v8::Isolate* isolate, SchedulerTaskContext* context);

  SchedulerTaskScope(const SchedulerTaskScope&) = delete;
  SchedulerTaskScope& operator=(const SchedulerTaskScope&) = delete;

  ~SchedulerTaskScope();

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const cppgc::Persistent<SchedulerTaskContext> previous_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCHEDULER_TASK_CONTEXT_H_
