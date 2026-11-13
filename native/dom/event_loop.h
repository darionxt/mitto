// The Page's event loop as the DOM sees it: the queues of Blink's main thread
// scheduler that a page's script can reach, each with its priority, and its
// idle tasks, which run in idle periods. The Page program makes it and gives
// it to the DOM, on its main thread.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/task_priority.h
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_scheduler_impl.cc
// Upstream: third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h

#ifndef MITTO_DOM_EVENT_LOOP_H_
#define MITTO_DOM_EVENT_LOOP_H_

#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"

namespace mitto {

class EventLoop {
 public:
  // A queue, by the priority that Blink's FrameSchedulerImpl gives it.
  enum class Queue {
    // Every task source of the HTML Standard that the Page has, at the normal
    // priority.
    kDefault,
    // scheduler.postTask() at each priority, and the continuations of
    // scheduler.yield(), which come before the tasks of their priority.
    kUserBlocking,
    kUserBlockingContinuation,
    kUserVisible,
    kUserVisibleContinuation,
    kBackground,
    kBackgroundContinuation,
  };

  // Runs in an idle period, and gets the period's end.
  using IdleTask = base::OnceCallback<void(base::TimeTicks deadline)>;

  // The Page's, or null before the Page program has made one.
  static EventLoop* Get();
  // Makes `event_loop` the Page's, or none for null.
  static void Set(EventLoop* event_loop);

  virtual scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(
      Queue queue) = 0;
  // Runs `task` when nothing else is ready, in the first idle period that
  // starts after now, as ThreadScheduler::PostIdleTask() does.
  virtual void PostIdleTask(const base::Location& from_here, IdleTask task) = 0;

 protected:
  virtual ~EventLoop() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_LOOP_H_
