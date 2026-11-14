#include "mitto/dom/animation_frame_controller.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/time_clamper.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// A Page is never cross-origin isolated.
constexpr bool kCrossOriginIsolated = false;

EventLoop& EventLoopOfPage() {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  return *event_loop;
}

// PageAnimator::ServiceScriptedAnimations()'s clamper, one for the process,
// with a secret apart from that of performance.now().
const TimeClamper& AnimationTimeClamper() {
  static const base::NoDestructor<TimeClamper> clamper;
  return *clamper;
}

}  // namespace

struct AnimationFrameController::FrameCallback {
  int32_t id = 0;
  bool is_cancelled = false;
  bool use_legacy_time_base = false;
  v8::Global<v8::Function> callback;
  // CaptureCurrentTaskState(): FrameRequestCallback supports task
  // attribution, so a yield() in the callback inherits what the task that
  // asked for it had.
  cppgc::Persistent<SchedulerTaskContext> task_state;
};

struct AnimationFrameController::QueuedEvent {
  cppgc::Persistent<EventTarget> target;
  cppgc::Persistent<Event> event;
};

AnimationFrameController::AnimationFrameController(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context)
    : isolate_(isolate),
      context_(std::make_unique<v8::Global<v8::Context>>(isolate, context)) {}

AnimationFrameController::~AnimationFrameController() = default;

// FrameRequestCallbackCollection::RegisterFrameCallback(), then
// ScriptedAnimationController::ScheduleAnimationIfNeeded().
int32_t AnimationFrameController::RequestAnimationFrame(
    v8::Local<v8::Function> callback,
    bool legacy) {
  auto frame_callback = std::make_unique<FrameCallback>();
  // ++next_callback_id_, an int, from 1 up.
  last_id_ = static_cast<int32_t>(static_cast<uint32_t>(last_id_) + 1);
  frame_callback->id = last_id_;
  frame_callback->use_legacy_time_base = legacy;
  frame_callback->callback.Reset(isolate_, callback);
  frame_callback->task_state = SchedulerTaskContext::Current(isolate_);
  callbacks_.push_back(std::move(frame_callback));
  ScheduleAnimationIfNeeded();
  return last_id_;
}

// FrameRequestCallbackCollection::CancelFrameCallback(): one that waits goes,
// and one that the current frame has yet to run is skipped.
void AnimationFrameController::CancelAnimationFrame(int32_t id) {
  const auto waiting = std::ranges::find_if(
      callbacks_, [id](const auto& callback) { return callback->id == id; });
  if (waiting != callbacks_.end()) {
    callbacks_.erase(waiting);
    return;
  }
  for (const std::unique_ptr<FrameCallback>& callback : callbacks_to_invoke_) {
    if (callback->id == id) {
      callback->is_cancelled = true;
      return;
    }
  }
}

void AnimationFrameController::EnqueueEvent(EventTarget& target, Event& event) {
  events_.push_back(std::make_unique<QueuedEvent>(
      QueuedEvent{.target = &target, .event = &event}));
  ScheduleAnimationIfNeeded();
}

void AnimationFrameController::ScheduleAnimationIfNeeded() {
  if (frame_requested_ || (callbacks_.empty() && events_.empty())) {
    return;
  }
  frame_requested_ = true;
  EventLoopOfPage().RequestBeginFrame(
      base::BindOnce(&AnimationFrameController::ServiceScriptedAnimations,
                     weak_factory_.GetWeakPtr()));
}

// PageAnimator::ServiceScriptedAnimations(): the document's animation clock
// takes the frame's time since its zero, the start of the navigation that
// loaded it, coarsened, and never goes back. A callback gets that in
// milliseconds, and a legacy one that time on the wall clock.
void AnimationFrameController::ServiceScriptedAnimations(
    base::TimeTicks frame_time) {
  frame_requested_ = false;
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_->Get(isolate_));
  DocumentLoadTiming& timing = Window::Current().GetDocumentLoadTiming();
  const base::TimeTicks zero_time = timing.navigation_start;
  if (frame_time > zero_time) {
    animation_time_ = std::max(
        animation_time_, AnimationTimeClamper().ClampTimeResolution(
                             frame_time - zero_time, kCrossOriginIsolated));
  }
  const base::WeakPtr<AnimationFrameController> self =
      weak_factory_.GetWeakPtr();
  DispatchEvents();
  // A listener may have ended the window's context, and this with it.
  if (!self) {
    return;
  }
  ExecuteFrameCallbacks(
      animation_time_.InMillisecondsF(),
      timing.MonotonicTimeToPseudoWallTime(zero_time + animation_time_)
          .InMillisecondsF());
}

// ScriptedAnimationController::DispatchEvents(): the events that wait now,
// each at its target, with its microtasks run before the next.
void AnimationFrameController::DispatchEvents() {
  std::vector<std::unique_ptr<QueuedEvent>> events;
  events.swap(events_);
  const v8::Local<v8::Context> context = context_->Get(isolate_);
  const base::WeakPtr<AnimationFrameController> self =
      weak_factory_.GetWeakPtr();
  for (const std::unique_ptr<QueuedEvent>& queued : events) {
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    queued->target->DispatchEvent(queued->event.Get());
    if (!self) {
      return;
    }
  }
}

// FrameRequestCallbackCollection::ExecuteFrameCallbacks(): the callbacks that
// wait now, each with undefined for `this`, and what it throws reported as an
// error that nothing caught. Its microtasks run before the next callback.
void AnimationFrameController::ExecuteFrameCallbacks(
    double high_res_now_ms,
    double high_res_now_ms_legacy) {
  callbacks_to_invoke_.swap(callbacks_);
  const v8::Local<v8::Context> context = context_->Get(isolate_);
  const base::WeakPtr<AnimationFrameController> self =
      weak_factory_.GetWeakPtr();
  // A callback may cancel a later one, so each is read from the list in turn.
  for (size_t i = 0; i < callbacks_to_invoke_.size(); ++i) {
    FrameCallback& frame_callback = *callbacks_to_invoke_[i];
    if (frame_callback.is_cancelled) {
      continue;
    }
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    const SchedulerTaskScope task_scope(isolate_,
                                        frame_callback.task_state.Get());
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::Value> argument = v8::Number::New(
        isolate_, frame_callback.use_legacy_time_base ? high_res_now_ms_legacy
                                                      : high_res_now_ms);
    std::ignore = frame_callback.callback.Get(isolate_)->Call(
        context, v8::Undefined(isolate_), 1, &argument);
    if (try_catch.HasCaught() && try_catch.CanContinue()) {
      bindings::ReportException(isolate_, try_catch.Exception());
    }
    // The callback may have ended the window's context, and this with it.
    if (!self) {
      return;
    }
  }
  callbacks_to_invoke_.clear();
}

}  // namespace mitto
