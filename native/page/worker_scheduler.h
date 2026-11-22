// A dedicated worker's thread scheduler, its event loop, as Blink's
// WorkerThreadScheduler and WorkerSchedulerImpl make it: a SequenceManager
// that the creator's thread makes, so that it can post to the worker at once,
// and that the worker's thread binds. A worker's tasks go to Blink's four
// queues of a worker, throttleable, pausable, pausable without virtual time
// and unpausable, all at the normal priority, so they run in the order they
// were ready whichever queue holds them, beside a default queue, V8's, a
// control queue before them, the queues of scheduler.postTask(), and the
// compositor's queue, which its BeginFrames come on. Nothing throttles a
// worker, as BlinkSchedulerWorkerThrottling is off.
//
// Upstream: third_party/blink/renderer/platform/scheduler/worker/worker_thread_scheduler.cc
// Upstream: third_party/blink/renderer/platform/scheduler/worker/worker_scheduler_impl.cc
// Upstream: third_party/blink/renderer/platform/scheduler/worker/non_main_thread_task_queue.cc
// Upstream: third_party/blink/renderer/platform/scheduler/worker/non_main_thread_scheduler_helper.cc

#ifndef MITTO_PAGE_WORKER_SCHEDULER_H_
#define MITTO_PAGE_WORKER_SCHEDULER_H_

#include <stddef.h>

#include <array>
#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"
#include "mitto/page/worker_begin_frame_provider.h"

namespace mitto {

class WorkerScheduler final : public EventLoop {
 public:
  // Blink's queues of a worker, by their traits.
  enum class WorkerQueue {
    kThrottleable,
    kPausable,
    kPausableNonVirtualTime,
    kUnpausable,
  };

  // On the creator's thread: the scheduler and its queues, not yet bound to
  // a thread, whose task runners take tasks from now on.
  WorkerScheduler();

  WorkerScheduler(const WorkerScheduler&) = delete;
  WorkerScheduler& operator=(const WorkerScheduler&) = delete;

  // On the worker's thread, after Shutdown().
  ~WorkerScheduler() override;

  // On the worker's thread: binds it there, and makes it that thread's event
  // loop.
  void BindToCurrentThread();
  // On the worker's thread: shuts every queue down, so that what waits there
  // never runs, as WorkerSchedulerImpl::Dispose() does once a worker closes
  // or ends.
  void Shutdown();

  // The default queue's task runner, for what belongs to no task type.
  scoped_refptr<base::SingleThreadTaskRunner> DefaultTaskRunner() const;
  // V8's own tasks.
  scoped_refptr<base::SingleThreadTaskRunner> V8TaskRunner() const;
  // The control queue's, before every other.
  scoped_refptr<base::SingleThreadTaskRunner> ControlTaskRunner() const;

  // EventLoop:
  scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(
      TaskType type) override;
  scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(Queue queue) override;
  void PostIdleTask(const base::Location& from_here, IdleTask task) override;
  void RequestBeginFrame(BeginFrameCallback begin_frame) override;
  void SetRefreshRate(int hertz) override;

 private:
  static constexpr size_t kWebSchedulingQueueCount =
      static_cast<size_t>(Queue::kBackgroundContinuation) + 1;
  static constexpr size_t kWorkerQueueCount =
      static_cast<size_t>(WorkerQueue::kUnpausable) + 1;

  std::unique_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  base::sequence_manager::TaskQueue::Handle default_queue_;
  base::sequence_manager::TaskQueue::Handle v8_queue_;
  base::sequence_manager::TaskQueue::Handle control_queue_;
  base::sequence_manager::TaskQueue::Handle idle_queue_;
  base::sequence_manager::TaskQueue::Handle compositor_queue_;
  std::array<base::sequence_manager::TaskQueue::Handle, kWorkerQueueCount>
      worker_queues_;
  std::array<base::sequence_manager::TaskQueue::Handle,
             kWebSchedulingQueueCount>
      web_scheduling_queues_;
  // Their task runners, which stay once the queues have shut down, and take
  // nothing from then on.
  scoped_refptr<base::SingleThreadTaskRunner> default_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> v8_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> control_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> idle_runner_;
  // Made on the worker's thread, with the first frame that it asks for.
  std::unique_ptr<WorkerBeginFrameProvider> begin_frames_;
  std::array<scoped_refptr<base::SingleThreadTaskRunner>, kWorkerQueueCount>
      worker_runners_;
  std::array<scoped_refptr<base::SingleThreadTaskRunner>,
             kWebSchedulingQueueCount>
      web_scheduling_runners_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_WORKER_SCHEDULER_H_
