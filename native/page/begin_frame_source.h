// The frames that the Page begins, as Chrome on Windows begins them for a
// visible page that asks for one: at the vsyncs of the display, each a period
// apart that Chrome works out from the display's refresh rate.
//
// A frame that is asked for while frames are not coming is a missed one, as
// Viz sends a compositor that starts to observe it: it begins at once, with the
// time of the last vsync. While they come, each frame begins with the first
// BeginFrame that reaches the compositor after the last frame ended, and they
// stop coming with the next one after a frame that asked for no other. Each
// frame runs as a task of the compositor's queue, which waits behind a frame
// that has not run yet, so a busy main thread runs a frame late with the time
// of the vsync it was sent for. The queue has the normal priority, and a very
// high one once a task ends 100 milliseconds or more after the last frame,
// until the next frame, as Blink keeps rendering from starving. The idle
// periods end as a frame begins, and a short one lasts until the next vsync
// after it, until long ones start again once frames stop.
//
// Upstream: ui/gl/vsync_provider_win.cc
// Upstream: ui/gl/vsync_thread_win.cc
// Upstream: components/viz/service/frame_sinks/external_begin_frame_source_win.cc
// Upstream: components/viz/common/frame_sinks/begin_frame_source.cc
// Upstream: cc/scheduler/scheduler.cc
// Upstream: cc/scheduler/scheduler_state_machine.cc
// Upstream: cc/trees/proxy_main.cc
// Upstream: third_party/blink/renderer/platform/scheduler/main_thread/main_thread_scheduler_impl.cc

#ifndef MITTO_PAGE_BEGIN_FRAME_SOURCE_H_
#define MITTO_PAGE_BEGIN_FRAME_SOURCE_H_

#include <vector>

#include "base/cancelable_callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/task_observer.h"
#include "base/time/time.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

class IdleHelper;

class BeginFrameSource final : public base::TaskObserver {
 public:
  // The priorities of the compositor's queue: its own, and the one that it
  // has while rendering starves.
  struct Priorities {
    base::sequence_manager::TaskQueue::QueuePriority normal;
    base::sequence_manager::TaskQueue::QueuePriority starved;
  };

  // Runs frames on `compositor_queue` of `sequence_manager`, and starts and
  // ends `idle_helper`'s idle periods. All of them must outlive it.
  BeginFrameSource(base::sequence_manager::SequenceManager* sequence_manager,
                   base::sequence_manager::TaskQueue* compositor_queue,
                   Priorities priorities,
                   IdleHelper* idle_helper);

  BeginFrameSource(const BeginFrameSource&) = delete;
  BeginFrameSource& operator=(const BeginFrameSource&) = delete;

  ~BeginFrameSource() override;

  // Sets the display's refresh rate, from 1 to 1000 hertz.
  void SetRefreshRate(int hertz);
  void RequestBeginFrame(EventLoop::BeginFrameCallback begin_frame);

  // base::TaskObserver:
  void WillProcessTask(const base::PendingTask& pending_task,
                       bool was_blocked_or_low_priority) override;
  void DidProcessTask(const base::PendingTask& pending_task) override;

 private:
  // The vsync at or before `time`, and the first one after it.
  base::TimeTicks LastVSyncAtOrBefore(base::TimeTicks time) const;
  base::TimeTicks NextVSyncAfter(base::TimeTicks time) const;
  // The vsync of the first BeginFrame after the last frame's that reaches the
  // compositor after `time`.
  base::TimeTicks NextBeginFrameAfter(base::TimeTicks time) const;
  // Posts the next frame, for the vsync at `frame_time`, to arrive at
  // `arrival`.
  void PostFrame(base::TimeTicks frame_time, base::TimeTicks arrival);
  void BeginFrame(base::TimeTicks frame_time);
  // At the vsync after a frame that asked for no other.
  void StopObservingIfIdle();

  const raw_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  const raw_ptr<base::sequence_manager::TaskQueue> compositor_queue_;
  const Priorities priorities_;
  const raw_ptr<IdleHelper> idle_helper_;
  // A vsync, and the period between two.
  base::TimeTicks vsync_timebase_;
  base::TimeDelta vsync_interval_;
  // What the next frame runs.
  std::vector<EventLoop::BeginFrameCallback> requests_;
  // Whether frames come, at each vsync, as when a compositor observes them.
  bool observing_ = false;
  // Whether a frame has been posted and has not begun.
  bool frame_posted_ = false;
  // Whether a frame runs its callbacks now.
  bool in_frame_ = false;
  // The time of the last frame that began.
  base::TimeTicks last_frame_time_;
  // When the last frame's task ended, and whether the current task is a frame.
  base::TimeTicks last_frame_end_;
  bool current_task_is_frame_ = false;
  bool rendering_starved_ = false;
  base::CancelableOnceClosure stop_observing_;
  base::WeakPtrFactory<BeginFrameSource> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_BEGIN_FRAME_SOURCE_H_
