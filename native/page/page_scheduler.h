// The Page's main thread scheduler: its event loop, a SequenceManager on the
// main thread with a queue for each priority that Blink's main thread
// scheduler gives what a page's script can reach, a control queue before them
// all, the compositor's queue, where frames begin, and an idle queue after
// them, whose idle periods start as soon as the Page runs, as Blink's do once
// a page expects no frames. Tasks run in the order of their queues'
// priorities, and in the order they were ready within one.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/task_priority.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/main_thread_scheduler_impl.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_scheduler_impl.cc
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
#include "mitto/page/begin_frame_source.h"
#include "mitto/page/idle_helper.h"

namespace mitto {

class PageScheduler final : public EventLoop {
 public:
  // Binds itself to the current thread, whose default task runner is its
  // default queue's, and becomes the DOM's event loop.
  PageScheduler();

  PageScheduler(const PageScheduler&) = delete;
  PageScheduler& operator=(const PageScheduler&) = delete;

  ~PageScheduler() override;

  // The default queue's task runner.
  const scoped_refptr<base::SingleThreadTaskRunner>& task_runner() const;

  // EventLoop:
  scoped_refptr<base::SingleThreadTaskRunner> TaskRunner(Queue queue) override;
  void PostIdleTask(const base::Location& from_here, IdleTask task) override;
  void RequestBeginFrame(BeginFrameCallback begin_frame) override;
  void SetRefreshRate(int hertz) override;

 private:
  static constexpr size_t kQueueCount =
      static_cast<size_t>(Queue::kBackgroundContinuation) + 1;

  std::unique_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  std::array<base::sequence_manager::TaskQueue::Handle, kQueueCount> queues_;
  base::sequence_manager::TaskQueue::Handle control_queue_;
  base::sequence_manager::TaskQueue::Handle compositor_queue_;
  base::sequence_manager::TaskQueue::Handle idle_queue_;
  std::unique_ptr<IdleHelper> idle_helper_;
  std::unique_ptr<BeginFrameSource> begin_frame_source_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_SCHEDULER_H_
