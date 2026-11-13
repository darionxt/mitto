#include "mitto/page/begin_frame_source.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/page/idle_helper.h"

namespace mitto {
namespace {

// What a refresh rate of none would give: BeginFrameArgs::DefaultInterval()'s
// 60 frames a second.
constexpr int kDefaultRefreshRate = 60;
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
  sequence_manager_->AddTaskObserver(this);
  SetRefreshRate(kDefaultRefreshRate);
}

BeginFrameSource::~BeginFrameSource() {
  sequence_manager_->RemoveTaskObserver(this);
}

void BeginFrameSource::SetRefreshRate(int hertz) {
  CHECK_GE(hertz, 1);
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

void BeginFrameSource::RequestBeginFrame(
    EventLoop::BeginFrameCallback begin_frame) {
  requests_.push_back(std::move(begin_frame));
  // A frame that has been posted runs it, and one that runs now posts the next
  // once it ends.
  if (frame_posted_ || in_frame_) {
    return;
  }
  stop_observing_.Cancel();
  const base::TimeTicks now = base::TimeTicks::Now();
  if (observing_) {
    // A BeginFrame comes at each vsync, and the next sends a main frame.
    const base::TimeTicks frame_time = NextVSyncAfter(now);
    PostFrame(frame_time, frame_time + kBeginFrameLatency);
    return;
  }
  // ExternalBeginFrameSource::AddObserver() sends a missed BeginFrame, which
  // ExternalBeginFrameSourceWin::GetMissedBeginFrameArgs() gives the time of
  // the last vsync, and only once for each vsync. cc's Scheduler takes it,
  // unless its deadline, the next vsync, has passed by the time it arrives.
  observing_ = true;
  const base::TimeTicks arrival = now + kMissedBeginFrameLatency;
  const base::TimeTicks missed = LastVSyncAtOrBefore(now);
  if (missed > last_frame_time_ && missed + vsync_interval_ >= arrival) {
    PostFrame(missed, arrival);
    return;
  }
  const base::TimeTicks frame_time = NextVSyncAfter(now);
  PostFrame(frame_time, frame_time + kBeginFrameLatency);
}

void BeginFrameSource::PostFrame(base::TimeTicks frame_time,
                                 base::TimeTicks arrival) {
  frame_posted_ = true;
  compositor_queue_->task_runner()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&BeginFrameSource::BeginFrame, weak_factory_.GetWeakPtr(),
                     frame_time),
      std::max(base::TimeDelta(), arrival - base::TimeTicks::Now()));
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
  const base::TimeTicks next_vsync = NextVSyncAfter(now);
  if (!requests_.empty()) {
    // cc sends the next main frame with the first BeginFrame after this one
    // ends, and DidCommitFrameToCompositor() starts a short idle period until
    // the next frame is expected.
    PostFrame(next_vsync, next_vsync + kBeginFrameLatency);
    if (now < estimated_next_frame_begin) {
      idle_helper_->StartShortIdlePeriod(now, estimated_next_frame_begin);
    }
    return;
  }
  // ProxyMain::MaybeIdleMainThread(): no main frame is expected until the next
  // vsync, and BeginFrames stop then, with BeginFrameNotExpectedSoon().
  if (now < next_vsync) {
    idle_helper_->StartShortIdlePeriod(now, next_vsync);
  }
  stop_observing_.Reset(base::BindOnce(&BeginFrameSource::StopObservingIfIdle,
                                       weak_factory_.GetWeakPtr()));
  compositor_queue_->task_runner()->PostDelayedTask(
      FROM_HERE, stop_observing_.callback(),
      next_vsync + kBeginFrameLatency - now);
}

void BeginFrameSource::StopObservingIfIdle() {
  if (frame_posted_ || !requests_.empty()) {
    return;
  }
  observing_ = false;
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
