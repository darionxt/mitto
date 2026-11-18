// The frames of a dedicated worker's requestAnimationFrame(), as Blink's
// BeginFrameProvider has Viz send BeginFrames to a compositor frame sink of the
// worker's own: at the display's vsyncs, those of the Page's frames, each as a
// task of the worker's compositor queue, about as long after its vsync as the
// Page's frames start. The first after a time without them is a missed one,
// which comes at once with the time of the last vsync, as Viz sends one to an
// observer that starts, and they stop with the first that nothing waits for.
// One that comes after its deadline, the next vsync, is dropped, as
// BeginFrameProvider drops a late one, and the frame waits for the next.
//
// Upstream: third_party/blink/renderer/platform/graphics/begin_frame_provider.cc
// Upstream: components/viz/common/frame_sinks/begin_frame_source.cc

#ifndef MITTO_PAGE_WORKER_BEGIN_FRAME_PROVIDER_H_
#define MITTO_PAGE_WORKER_BEGIN_FRAME_PROVIDER_H_

#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

class WorkerBeginFrameProvider {
 public:
  // On the worker's thread, whose BeginFrames are tasks of `compositor`.
  explicit WorkerBeginFrameProvider(
      scoped_refptr<base::SingleThreadTaskRunner> compositor);

  WorkerBeginFrameProvider(const WorkerBeginFrameProvider&) = delete;
  WorkerBeginFrameProvider& operator=(const WorkerBeginFrameProvider&) = delete;

  ~WorkerBeginFrameProvider();

  // Runs `begin_frame` in the next frame, with its vsync's time.
  void RequestBeginFrame(EventLoop::BeginFrameCallback begin_frame);

 private:
  // The BeginFrame of the vsync at `frame_time`, which arrives at `arrival`.
  void PostBeginFrame(base::TimeTicks frame_time, base::TimeTicks arrival);
  void OnBeginFrame(base::TimeTicks frame_time);

  const scoped_refptr<base::SingleThreadTaskRunner> compositor_;
  base::DeadlineTimer timer_;
  // What the next frame runs.
  std::vector<EventLoop::BeginFrameCallback> requests_;
  // Whether BeginFrames come, as when the frame sink needs them.
  bool observing_ = false;
};

}  // namespace mitto

#endif  // MITTO_PAGE_WORKER_BEGIN_FRAME_PROVIDER_H_
