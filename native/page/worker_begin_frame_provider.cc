#include "mitto/page/worker_begin_frame_provider.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "mitto/page/begin_frame_source.h"

namespace mitto {
namespace {

// How long after its vsync a BeginFrame reaches the worker: Chrome's measured
// latency, but within half of a display's period that is shorter, so that each
// frame arrives before its deadline, the next vsync.
base::TimeDelta ArrivalLatency(const BeginFrameSource::VSync& vsync) {
  return std::min(BeginFrameSource::kBeginFrameLatency, vsync.interval / 2);
}

}  // namespace

WorkerBeginFrameProvider::WorkerBeginFrameProvider(
    scoped_refptr<base::SingleThreadTaskRunner> compositor)
    : compositor_(std::move(compositor)) {
  timer_.SetTaskRunner(compositor_);
}

WorkerBeginFrameProvider::~WorkerBeginFrameProvider() = default;

// BeginFrameProvider::RequestBeginFrame(): the frame sink starts to need
// BeginFrames, and Viz sends a missed one at once.
void WorkerBeginFrameProvider::RequestBeginFrame(
    EventLoop::BeginFrameCallback begin_frame) {
  requests_.push_back(std::move(begin_frame));
  if (observing_) {
    return;
  }
  observing_ = true;
  const base::TimeTicks now = base::TimeTicks::Now();
  PostBeginFrame(BeginFrameSource::DisplayVSync().LastAtOrBefore(now),
                 now + BeginFrameSource::kMissedBeginFrameLatency);
}

void WorkerBeginFrameProvider::PostBeginFrame(base::TimeTicks frame_time,
                                              base::TimeTicks arrival) {
  timer_.Start(FROM_HERE, arrival,
               base::BindOnce(&WorkerBeginFrameProvider::OnBeginFrame,
                              base::Unretained(this), frame_time),
               base::subtle::DelayPolicy::kPrecise);
}

// BeginFrameProvider::OnBeginFrame(): one that nothing waits for stops them,
// and a late one is dropped, though not two in a row: the one after is taken
// however late it comes, as cc takes the missed BeginFrame that viz sends a
// client that missed one, so that a thread that wakes late still gets frames.
// The rest run what waits, and the next comes at the next vsync.
void WorkerBeginFrameProvider::OnBeginFrame(base::TimeTicks frame_time) {
  if (requests_.empty()) {
    observing_ = false;
    return;
  }
  const BeginFrameSource::VSync vsync = BeginFrameSource::DisplayVSync();
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!dropped_last_ && now > frame_time + vsync.interval) {
    dropped_last_ = true;
    const base::TimeTicks next = vsync.NextAfter(now);
    PostBeginFrame(next, next + ArrivalLatency(vsync));
    return;
  }
  dropped_last_ = false;
  const base::TimeTicks next = vsync.NextAfter(frame_time);
  PostBeginFrame(next, next + ArrivalLatency(vsync));
  std::vector<EventLoop::BeginFrameCallback> requests;
  requests.swap(requests_);
  for (EventLoop::BeginFrameCallback& request : requests) {
    std::move(request).Run(frame_time);
  }
}

}  // namespace mitto
