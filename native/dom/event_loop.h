// The Page's event loop as the DOM sees it: a task runner for each of Blink's
// task types that a Page posts, on the queue and at the priority that Blink's
// frame scheduler gives that type, the queues of scheduler.postTask(), and
// idle tasks, which run in idle periods. The Page program makes it and gives
// it to the DOM, on its main thread.
//
// Upstream: third_party/blink/public/platform/task_type.h
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

// Blink's TaskType, for the task sources of the HTML Standard and the other
// specifications that a Page has, and Blink's own tasks among them. Each runs
// on the queue that FrameSchedulerImpl::CreateQueueTraitsForTaskType() gives
// it, at the priority of FrameSchedulerImpl::ComputePriority().
enum class TaskType {
  // https://html.spec.whatwg.org/multipage/webappapis.html#generic-task-sources
  kDOMManipulation,
  kUserInteraction,
  kNetworking,
  kNetworkingUnfreezable,
  // A response of a request that blocks rendering, such as an image's, at a
  // priority above every other task's of a page that is visible.
  kNetworkingUnfreezableRenderBlockingLoading,
  // What changes a request, at a high priority.
  kNetworkingControl,
  // An async script of another site, as Blink runs it while a page loads, at
  // a low priority.
  kLowPriorityScriptExecution,
  kHistoryTraversal,
  kEmbed,
  kMediaElementEvent,
  kCanvasBlobSerialization,
  kMicrotask,
  // A timer of no delay, one with a delay, and one with a delay that is
  // nested five or more deep.
  kJavascriptTimerImmediate,
  kJavascriptTimerDelayedLowNesting,
  kJavascriptTimerDelayedHighNesting,
  kRemoteEvent,
  kWebSocket,
  kPostedMessage,
  kUnshippedPortMessage,
  kFileReading,
  kDatabaseAccess,
  kPresentation,
  kSensor,
  kPerformanceTimeline,
  kWebGL,
  kWebGPU,
  // An idle callback that runs as a task once its timeout has passed.
  kIdleTask,
  kMiscPlatformAPI,
  kFontLoading,
  kApplicationLifeCycle,
  kBackgroundFetch,
  kPermission,
  kWebLocks,
  kStorage,
  kClipboard,
  kMachineLearning,
  // Blink's own tasks.
  kInternalDefault,
  kInternalLoading,
  kInternalTest,
  kInternalWebCrypto,
  // What DevTools asks of a page, as the scripts that the Session evaluates.
  kInternalInspector,
  // A parser that goes on once the script it waited for is ready, and the
  // next deferred script, at a very high priority.
  kInternalContinueScriptLoading,
  kInternalNavigationAssociated,
  // The main thread's compositor queue, where a frame's rendering steps
  // begin, at the normal priority while rendering is not starved.
  kMainThreadTaskQueueCompositor,
};

class EventLoop {
 public:
  // A queue of scheduler.postTask(), at each of its priorities, and the
  // continuations of scheduler.yield(), which come before the tasks of their
  // priority.
  enum class Queue {
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

  // The task runner of tasks of `type`.
  virtual scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(
      TaskType type) = 0;
  // The task runner of scheduler.postTask()'s `queue`.
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
