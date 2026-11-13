#include "mitto/dom/event.h"

#include <cmath>

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_event.h"
#include "mitto/dom/event_path.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/node.h"
#include "mitto/dom/window.h"
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
  visitor->Trace(underlying_event_);
  visitor->Trace(event_path_);
}

EventTarget* Event::Target() const {
  return target_.Get();
}

EventTarget* Event::CurrentTarget() const {
  return current_target_.Get();
}

std::vector<EventTarget*> Event::ComposedPath() const {
  // Blink's Event::composedPath(): nothing before or after a dispatch.
  if (!current_target_) {
    return {};
  }
  // At a node, the path as the node's tree sees it.
  if (Node* const node = current_target_->ToNode()) {
    if (event_path_) {
      for (size_t i = 0; i < event_path_->size(); ++i) {
        const NodeEventContext& context = event_path_->at(i);
        if (&context.GetNode() == node) {
          return context.GetTreeScopeEventContext().ComposedPath(*event_path_);
        }
      }
    }
    return {};
  }
  // At the window, the path of the tree of the path's topmost node.
  Window* const window = current_target_->ToWindow();
  if (window && event_path_ && !event_path_->IsEmpty()) {
    return event_path_->TopNodeEventContext()
        .GetTreeScopeEventContext()
        .ComposedPath(*event_path_);
  }
  // At a target that is neither a node nor a window, the target while it
  // dispatches the event. Without ComposedPathReturnTargetBeingDispatched,
  // which is on for stable, Blink gives one only to a window.
  const bool return_target =
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
              ->features()
              .IsEnabled(bindings::RuntimeFeature::
                             kComposedPathReturnTargetBeingDispatched)
          ? IsBeingDispatched()
          : window != nullptr;
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
  if (handling_passive_ != PassiveMode::kNotPassive &&
      handling_passive_ != PassiveMode::kNotPassiveDefault) {
    // Blink says so only for a listener that a page made passive, and only
    // where the dispatch has a window, which one through a document has.
    Window* const window =
        event_path_ && event_path_->HasWindowEventContext()
            ? event_path_->GetWindowEventContext().GetWindow()
            : nullptr;
    if (window && handling_passive_ == PassiveMode::kPassive) {
      window->AddConsoleMessage(
          ConsoleLevel::kError,
          "Unable to preventDefault inside passive event listener "
          "invocation.");
    }
    return;
  }
  if (cancelable_) {
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

bool Event::IsMouseEvent() const {
  return false;
}

bool Event::IsErrorEvent() const {
  return false;
}

void Event::SetTarget(EventTarget* target) {
  target_ = target;
}

void Event::SetCurrentTarget(EventTarget* target) {
  current_target_ = target;
}

void Event::SetUnderlyingEvent(const Event* event) {
  // No cycle, as in Blink.
  for (const Event* underlying = event; underlying;
       underlying = underlying->UnderlyingEvent()) {
    if (underlying == this) {
      return;
    }
  }
  underlying_event_ = event;
}

void Event::InitEventPath(Node& node) {
  if (copy_event_path_from_underlying_event_ && underlying_event_ &&
      underlying_event_->GetEventPath()) {
    event_path_ = underlying_event_->GetEventPath();
    return;
  }
  event_path_ = bindings::MakeGarbageCollected<EventPath>(node, this);
}

}  // namespace mitto
