// The Page's event loop as the DOM sees it: the queues of Blink's main thread
// scheduler that a page's script can reach, each with its priority, and when
// the next delayed task is due, which an idle period ends at. The Page
// program makes it and gives it to the DOM, on its main thread.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/task_priority.h
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_scheduler_impl.cc

#ifndef MITTO_DOM_EVENT_LOOP_H_
#define MITTO_DOM_EVENT_LOOP_H_

#include <optional>

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
    // Idle tasks, which run only when nothing else is ready, at the best
    // effort priority.
    kIdle,
  };

  // The Page's, or null before the Page program has made one.
  static EventLoop* Get();
  // Makes `event_loop` the Page's, or none for null.
  static void Set(EventLoop* event_loop);

  virtual scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(
      Queue queue) = 0;
  // When the next delayed task of any queue is due, if one is.
  virtual std::optional<base::TimeTicks> NextDelayedTaskTime() = 0;

 protected:
  virtual ~EventLoop() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_LOOP_H_
