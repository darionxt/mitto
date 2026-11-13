// The animation frame callbacks of a window's context: requestAnimationFrame(),
// webkitRequestAnimationFrame() and cancelAnimationFrame() as Chrome runs
// them. A callback runs in the next frame that the event loop begins, with the
// frame's time on the document's clock, coarsened, and every callback of a
// frame gets the same time. A callback asked for in a frame runs in the next.
// The Page lays nothing out, so these are the only rendering steps that its
// frames run.
//
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.cc
// Upstream: third_party/blink/renderer/core/dom/scripted_animation_controller.cc
// Upstream: third_party/blink/renderer/core/dom/frame_request_callback_collection.cc
// Upstream: third_party/blink/renderer/core/page/page_animator.cc

#ifndef MITTO_DOM_ANIMATION_FRAME_CONTROLLER_H_
#define MITTO_DOM_ANIMATION_FRAME_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class AnimationFrameController {
 public:
  // The callbacks run in `context`.
  AnimationFrameController(v8::Isolate* isolate,
                           v8::Local<v8::Context> context);

  AnimationFrameController(const AnimationFrameController&) = delete;
  AnimationFrameController& operator=(const AnimationFrameController&) = delete;

  ~AnimationFrameController();

  // Returns the callback's ID, from 1 up. A `legacy` callback, of
  // webkitRequestAnimationFrame(), gets the frame's time on the wall clock.
  int32_t RequestAnimationFrame(v8::Local<v8::Function> callback, bool legacy);
  void CancelAnimationFrame(int32_t id);

  // How many callbacks wait for a frame.
  size_t size() const { return callbacks_.size(); }

 private:
  struct FrameCallback;

  void ScheduleAnimationIfNeeded();
  void ServiceScriptedAnimations(base::TimeTicks frame_time);
  void ExecuteFrameCallbacks(double high_res_now_ms,
                             double high_res_now_ms_legacy);

  const raw_ptr<v8::Isolate> isolate_;
  const std::unique_ptr<v8::Global<v8::Context>> context_;
  // Those that wait for the next frame, and those that the current frame
  // runs.
  std::vector<std::unique_ptr<FrameCallback>> callbacks_;
  std::vector<std::unique_ptr<FrameCallback>> callbacks_to_invoke_;
  int32_t last_id_ = 0;
  bool frame_requested_ = false;
  // The document's animation clock, from its zero, which never goes back.
  base::TimeDelta animation_time_;
  base::WeakPtrFactory<AnimationFrameController> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_ANIMATION_FRAME_CONTROLLER_H_
