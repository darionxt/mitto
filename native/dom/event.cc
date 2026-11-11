#include "mitto/dom/event.h"

#include <cmath>

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_event.h"
#include "mitto/dom/event_target.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// Chrome coarsens time stamps to 100 microseconds for a context that is not
// cross-origin isolated, in Blink's TimeClamper, which also adds jitter. The
// Runtime does not add jitter yet.
constexpr double kResolutionMilliseconds = 0.1;

}  // namespace

// static
Event* Event::Create(const std::u16string& type,
                     const bindings::EventInit& init) {
  return bindings::MakeGarbageCollected<Event>(type, init);
}

Event::Event(const std::u16string& type, const bindings::EventInit& init)
    : type_(type),
      bubbles_(init.bubbles),
      cancelable_(init.cancelable),
      composed_(init.composed),
      created_(base::TimeTicks::Now()) {}

const bindings::WrapperTypeInfo* Event::GetWrapperTypeInfo() const {
  return &bindings::V8Event::kWrapperTypeInfo;
}

void Event::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(target_);
  visitor->Trace(current_target_);
}

EventTarget* Event::Target() const {
  return target_.Get();
}

EventTarget* Event::CurrentTarget() const {
  return current_target_.Get();
}

std::vector<EventTarget*> Event::ComposedPath() const {
  // Blink's Event::composedPath() for a target that is neither a node nor a
  // window, the only kind the Runtime has yet: the path is the target while it
  // dispatches the event, and without ComposedPathReturnTargetBeingDispatched,
  // which is on for stable, it is empty, since Blink then gives one only to a
  // window.
  if (!current_target_) {
    return {};
  }
  const bool return_target =
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
          ->features()
          .IsEnabled(bindings::RuntimeFeature::
                         kComposedPathReturnTargetBeingDispatched) &&
      IsBeingDispatched();
  if (!return_target) {
    return {};
  }
  return {current_target_.Get()};
}

void Event::StopImmediatePropagation() {
  stop_propagation_ = true;
  stop_immediate_propagation_ = true;
}

void Event::PreventDefault() {
  if (cancelable_ && !in_passive_listener_) {
    canceled_ = true;
  }
}

double Event::TimeStamp() const {
  const base::TimeTicks origin =
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->time_origin();
  const double milliseconds = (created_ - origin).InMillisecondsF();
  return std::floor(milliseconds / kResolutionMilliseconds) *
         kResolutionMilliseconds;
}

void Event::InitEvent(const std::u16string& type,
                      bool bubbles,
                      bool cancelable) {
  if (IsBeingDispatched()) {
    return;
  }
  stop_propagation_ = false;
  stop_immediate_propagation_ = false;
  canceled_ = false;
  trusted_ = false;
  type_ = type;
  bubbles_ = bubbles;
  cancelable_ = cancelable;
}

void Event::SetLegacyReturnValue(bool value) {
  if (!value) {
    PreventDefault();
  }
}

void Event::SetCancelBubble(bool value) {
  if (value) {
    stop_propagation_ = true;
  }
}

void Event::SetTarget(EventTarget* target) {
  target_ = target;
}

void Event::SetCurrentTarget(EventTarget* target) {
  current_target_ = target;
}

}  // namespace mitto
