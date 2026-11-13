// The Page's main thread scheduler: its event loop, a SequenceManager on the
// main thread with Blink's eleven task priorities. A page's tasks go to the
// queues that Blink's frame scheduler makes for the traits of each task type,
// each at the priority that it gives them, beside the queues of
// scheduler.postTask(), V8's queue, the compositor's, a default queue for
// what belongs to no frame, a control queue before them all and an idle queue
// after them, whose idle periods start as soon as the Page runs, as Blink's
// do once a page expects no frames. Tasks run in the order of their queues'
// priorities, and in the order they were ready among queues of one priority.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/task_priority.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/main_thread_scheduler_impl.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_scheduler_impl.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_task_queue_controller.cc
// Upstream: base/task/single_thread_task_executor.cc

#ifndef MITTO_PAGE_PAGE_SCHEDULER_H_
#define MITTO_PAGE_PAGE_SCHEDULER_H_

#include <array>
#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"
#include "mitto/page/idle_helper.h"

namespace mitto {

class PageScheduler final : public EventLoop {
 public:
  // The queues that Blink's FrameTaskQueueController makes for a frame, one
  // for each set of traits that its task types have, and those of the main
  // thread that a Page's tasks reach.
  enum class FrameQueue {
    kLoading,
    kLoadingControl,
    kRenderBlockingLoading,
    kLowPriorityScriptExecution,
    kThrottleable,
    kDeferrable,
    kPausable,
    kInternalScriptContinuation,
    kUnpausable,
    kFreezable,
    kInspector,
  };

  // Binds itself to the current thread, whose default task runner is its
  // default queue's, and becomes the DOM's event loop.
  PageScheduler();

  PageScheduler(const PageScheduler&) = delete;
  PageScheduler& operator=(const PageScheduler&) = delete;

  ~PageScheduler() override;

  // The default queue's task runner, for what belongs to no frame, as Blink's
  // main thread scheduler has one for IPC.
  const scoped_refptr<base::SingleThreadTaskRunner>& task_runner() const;
  // V8's own tasks, as gin posts them for Chrome's renderers.
  scoped_refptr<base::SingleThreadTaskRunner> V8TaskRunner() const;

  // EventLoop:
  scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(
      TaskType type) override;
  scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(Queue queue) override;
  void PostIdleTask(const base::Location& from_here, IdleTask task) override;

 private:
  static constexpr size_t kWebSchedulingQueueCount =
      static_cast<size_t>(Queue::kBackgroundContinuation) + 1;
  static constexpr size_t kFrameQueueCount =
      static_cast<size_t>(FrameQueue::kInspector) + 1;

  std::unique_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  base::sequence_manager::TaskQueue::Handle default_queue_;
  base::sequence_manager::TaskQueue::Handle v8_queue_;
  base::sequence_manager::TaskQueue::Handle compositor_queue_;
  std::array<base::sequence_manager::TaskQueue::Handle, kFrameQueueCount>
      frame_queues_;
  std::array<base::sequence_manager::TaskQueue::Handle,
             kWebSchedulingQueueCount>
      web_scheduling_queues_;
  base::sequence_manager::TaskQueue::Handle control_queue_;
  base::sequence_manager::TaskQueue::Handle idle_queue_;
  std::unique_ptr<IdleHelper> idle_helper_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_SCHEDULER_H_
