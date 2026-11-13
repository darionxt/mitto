#include "mitto/page/begin_frame_source.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "mitto/page/idle_helper.h"

namespace mitto {
namespace {

// What a refresh rate of none would give: BeginFrameArgs::DefaultInterval()'s
// 60 frames a second.
constexpr int kDefaultRefreshRate = 60;
// What a Machine's display can have. The period of the slowest is shorter than
// the longest idle period, so a short idle period never outlasts a long one.
constexpr int kLeastRefreshRate = 24;
constexpr int kMostRefreshRate = 1000;

// How long after its vsync a frame's callbacks start on an idle main thread,
// and how long after the request a missed frame's do, which no vsync waits
// for. Read from Chrome 157.0.8103.51 on macOS, headless: medians of 1.2 and
// 0.15 milliseconds.
constexpr base::TimeDelta kBeginFrameLatency = base::Microseconds(1200);
constexpr base::TimeDelta kMissedBeginFrameLatency = base::Microseconds(150);

// MainThreadSchedulerImpl's kDefaultRenderingStarvationThreshold.
constexpr base::TimeDelta kRenderingStarvationThreshold =
    base::Milliseconds(100);

}  // namespace

BeginFrameSource::BeginFrameSource(
    base::sequence_manager::SequenceManager* sequence_manager,
    base::sequence_manager::TaskQueue* compositor_queue,
    Priorities priorities,
    IdleHelper* idle_helper)
    : sequence_manager_(sequence_manager),
      compositor_queue_(compositor_queue),
      priorities_(priorities),
      idle_helper_(idle_helper) {
  compositor_queue_->SetQueuePriority(priorities_.normal);
  frame_timer_.SetTaskRunner(compositor_queue_->task_runner());
  stop_observing_timer_.SetTaskRunner(compositor_queue_->task_runner());
  sequence_manager_->AddTaskObserver(this);
  SetRefreshRate(kDefaultRefreshRate);
}

BeginFrameSource::~BeginFrameSource() {
  SetObserving(false);
  sequence_manager_->RemoveTaskObserver(this);
}

void BeginFrameSource::SetObserving(bool observing) {
  if (observing_ == observing) {
    return;
  }
  observing_ = observing;
#if BUILDFLAG(IS_WIN)
  // A precise timer then wakes within a millisecond, where it would wait for
  // the next tick of Windows' default timer.
  base::Time::EnableHighResolutionTimer(observing);
#endif
}

void BeginFrameSource::SetRefreshRate(int hertz) {
  CHECK_GE(hertz, kLeastRefreshRate);
  CHECK_LE(hertz, kMostRefreshRate);
  // TryGetVsyncIntervalFromDisplayConfig(): the period in whole microseconds.
  vsync_interval_ = base::Microseconds(base::ClampRound<int64_t>(
      static_cast<double>(base::Time::kMicrosecondsPerSecond) / hertz));
  // The display's vsyncs come at a phase of their own, which nothing that the
  // Page does sets.
  vsync_timebase_ =
      base::TimeTicks::Now() - vsync_interval_ * base::RandDouble();
}

base::TimeTicks BeginFrameSource::LastVSyncAtOrBefore(
    base::TimeTicks time) const {
  const base::TimeTicks next =
      time.SnappedToNextTick(vsync_timebase_, vsync_interval_);
  return next == time ? time : next - vsync_interval_;
}

base::TimeTicks BeginFrameSource::NextVSyncAfter(base::TimeTicks time) const {
  const base::TimeTicks next =
      time.SnappedToNextTick(vsync_timebase_, vsync_interval_);
  return next == time ? time + vsync_interval_ : next;
}

// A BeginFrame reaches the renderer's compositor about as long after its vsync
// as a frame takes to start, and the compositor sends a main frame with the
// first BeginFrame that reaches it once the last main frame has ended. So a
// display whose period is shorter than that still gets a frame at each vsync.
base::TimeTicks BeginFrameSource::NextBeginFrameAfter(
    base::TimeTicks time) const {
  return NextVSyncAfter(std::max(time - kBeginFrameLatency, last_frame_time_));
}

void BeginFrameSource::RequestBeginFrame(
    EventLoop::BeginFrameCallback begin_frame) {
  requests_.push_back(std::move(begin_frame));
  // A frame that has been posted runs it, and one that runs now posts the next
  // once it ends.
  if (frame_posted_ || in_frame_) {
    return;
  }
  stop_observing_timer_.Stop();
  const base::TimeTicks now = base::TimeTicks::Now();
  if (observing_) {
    // A BeginFrame comes at each vsync, and the next sends a main frame.
    const base::TimeTicks frame_time = NextBeginFrameAfter(now);
    PostFrame(frame_time, frame_time + kBeginFrameLatency);
    return;
  }
  // ExternalBeginFrameSource::AddObserver() sends a missed BeginFrame, which
  // ExternalBeginFrameSourceWin::GetMissedBeginFrameArgs() gives the time of
  // the last vsync, and only once for each vsync. cc's Scheduler takes it,
  // unless its deadline, the next vsync, has passed by the time it arrives.
  SetObserving(true);
  const base::TimeTicks arrival = now + kMissedBeginFrameLatency;
  const base::TimeTicks missed = LastVSyncAtOrBefore(now);
  if (missed > last_frame_time_ && missed + vsync_interval_ >= arrival) {
    PostFrame(missed, arrival);
    return;
  }
  const base::TimeTicks frame_time = NextBeginFrameAfter(now);
  PostFrame(frame_time, frame_time + kBeginFrameLatency);
}

void BeginFrameSource::PostFrame(base::TimeTicks frame_time,
                                 base::TimeTicks arrival) {
  frame_posted_ = true;
  frame_timer_.Start(FROM_HERE, arrival,
                     base::BindOnce(&BeginFrameSource::BeginFrame,
                                    base::Unretained(this), frame_time),
                     base::subtle::DelayPolicy::kPrecise);
}

void BeginFrameSource::BeginFrame(base::TimeTicks frame_time) {
  frame_posted_ = false;
  current_task_is_frame_ = true;
  // MainThreadSchedulerImpl::WillBeginFrame(): the idle period ends, and the
  // next frame is expected a vsync later.
  idle_helper_->EndIdlePeriod();
  const base::TimeTicks estimated_next_frame_begin =
      frame_time + vsync_interval_;
  last_frame_time_ = frame_time;
  std::vector<EventLoop::BeginFrameCallback> requests;
  requests.swap(requests_);
  in_frame_ = true;
  for (EventLoop::BeginFrameCallback& request : requests) {
    std::move(request).Run(frame_time);
  }
  in_frame_ = false;
  const base::TimeTicks now = base::TimeTicks::Now();
  const base::TimeTicks next_begin_frame = NextBeginFrameAfter(now);
  if (!requests_.empty()) {
    // The next main frame goes with the next BeginFrame, and
    // DidCommitFrameToCompositor() starts a short idle period until the next
    // frame is expected.
    PostFrame(next_begin_frame, next_begin_frame + kBeginFrameLatency);
    if (now < estimated_next_frame_begin) {
      idle_helper_->StartShortIdlePeriod(now, estimated_next_frame_begin);
    }
    return;
  }
  // ProxyMain::MaybeIdleMainThread(): no main frame is expected until the next
  // vsync, and BeginFrames stop with the next BeginFrame, which finds none
  // asked for, with BeginFrameNotExpectedSoon().
  const base::TimeTicks next_vsync = NextVSyncAfter(now);
  if (now < next_vsync) {
    idle_helper_->StartShortIdlePeriod(now, next_vsync);
  }
  stop_observing_timer_.Start(
      FROM_HERE, next_begin_frame + kBeginFrameLatency,
      base::BindOnce(&BeginFrameSource::StopObservingIfIdle,
                     base::Unretained(this)),
      base::subtle::DelayPolicy::kPrecise);
}

void BeginFrameSource::StopObservingIfIdle() {
  if (frame_posted_ || !requests_.empty()) {
    return;
  }
  SetObserving(false);
  idle_helper_->EnableLongIdlePeriod();
}

void BeginFrameSource::WillProcessTask(const base::PendingTask& pending_task,
                                       bool was_blocked_or_low_priority) {
  current_task_is_frame_ = false;
}

// MainThreadSchedulerImpl::UpdateRenderingPrioritizationStateOnTaskCompleted():
// a frame ends the starvation, and any other task that ends long enough after
// the last frame, or before any frame, starts it.
void BeginFrameSource::DidProcessTask(const base::PendingTask& pending_task) {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (current_task_is_frame_) {
    last_frame_end_ = now;
    if (rendering_starved_) {
      rendering_starved_ = false;
      compositor_queue_->SetQueuePriority(priorities_.normal);
    }
    return;
  }
  if (!rendering_starved_ &&
      now - last_frame_end_ >= kRenderingStarvationThreshold) {
    rendering_starved_ = true;
    compositor_queue_->SetQueuePriority(priorities_.starved);
  }
}

}  // namespace mitto
