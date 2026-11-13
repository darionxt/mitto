// The Page's main thread scheduler: its event loop, a SequenceManager on the
// main thread with a queue for each priority that Blink's main thread
// scheduler gives what a page's script can reach. Tasks run in the order of
// their queues' priorities, and in the order they were ready within one.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/task_priority.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/frame_scheduler_impl.cc
// Upstream: base/task/single_thread_task_executor.cc

#ifndef MITTO_PAGE_PAGE_SCHEDULER_H_
#define MITTO_PAGE_PAGE_SCHEDULER_H_

#include <array>
#include <memory>
#include <optional>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mitto/dom/event_loop.h"

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
  std::optional<base::TimeTicks> NextDelayedTaskTime() override;

 private:
  static constexpr size_t kQueueCount = static_cast<size_t>(Queue::kIdle) + 1;

  std::unique_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  std::array<base::sequence_manager::TaskQueue::Handle, kQueueCount> queues_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_SCHEDULER_H_
