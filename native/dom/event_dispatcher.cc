#include "mitto/dom/event_dispatcher.h"

#include <algorithm>
#include <vector>

#include "base/check.h"
#include "base/no_destructor.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_path.h"
#include "mitto/dom/node.h"
#include "mitto/dom/objects_behind.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// Blink's FindActivationTarget(): the target if it has activation behaviour,
// or else, for an event that bubbles, the nearest ancestor on the path that
// has one.
Node* FindActivationTarget(Node& node, const Event& event) {
  if (node.HasActivationBehavior()) {
    return &node;
  }
  if (!event.Bubbles()) {
    return nullptr;
  }
  const EventPath& path = *event.GetEventPath();
  for (size_t i = 1; i < path.size(); ++i) {
    Node& target = path.at(i).GetNode();
    if (target.HasActivationBehavior()) {
      return &target;
    }
  }
  return nullptr;
}

bool IsActivationFeatureOn(bindings::RuntimeFeature feature) {
  return bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
      ->features()
      .IsEnabled(feature);
}

// The nodes in the middle of a simulated click, which get no other. Each is
// taken out again before its click returns, while the caller holds it.
std::vector<const Node*>& NodesDispatchingSimulatedClicks() {
  static base::NoDestructor<std::vector<const Node*>> nodes;
  return *nodes;
}

// Blink's SimulatedEventUtil::CreateEvent() for script's click(): a
// PointerEvent "click" that bubbles, can be canceled and is composed, whose
// view is the window, with no button or key down and the pointer ID that
// Blink reserves for an event of no pointer, -1, which no person started.
Event* CreateSimulatedClick(Node& node) {
  bindings::EventInit init;
  init.bubbles = true;
  init.cancelable = true;
  init.composed = true;
  Event* const event = CreateEventOfInterface(
      bindings::V8PointerEvent::kWrapperTypeInfo, u"click", init);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  // The attributes that PointerEvent's constructor would set from this
  // PointerEventInit, which are its own Known Gaps, read as what it set.
  const v8::Local<v8::Object> dictionary = v8::Object::New(isolate);
  Window* const window = node.GetDocument().DefaultView();
  const v8::Local<v8::Value> view =
      window ? window->ToV8(isolate) : v8::Null(isolate).As<v8::Value>();
  if (dictionary
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "view"), view)
          .IsNothing() ||
      dictionary
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "pointerId"),
                v8::Integer::New(isolate, -1))
          .IsNothing()) {
    return event;
  }
  const v8::Local<v8::Value> wrapper = event->ToV8(isolate);
  if (wrapper->IsObject()) {
    bindings::InitializeEventOfInterface(
        isolate, wrapper.As<v8::Object>(),
        bindings::V8PointerEvent::kWrapperTypeInfo, dictionary);
  }
  return event;
}

}  // namespace

// static
DispatchEventResult EventDispatcher::DispatchEvent(Node& node, Event& event) {
  EventDispatcher dispatcher(node, event);
  return dispatcher.Dispatch();
}

EventDispatcher::EventDispatcher(Node& node, Event& event)
    : node_(&node), event_(&event) {
  event_->InitEventPath(*node_);
}

EventDispatcher::~EventDispatcher() = default;

// static
void EventDispatcher::DispatchSimulatedClick(Node& node) {
  if (node.IsDisabledFormControl()) {
    return;
  }
  std::vector<const Node*>& nodes = NodesDispatchingSimulatedClicks();
  if (std::find(nodes.begin(), nodes.end(), &node) != nodes.end()) {
    return;
  }
  nodes.push_back(&node);
  const cppgc::Persistent<Event> click(CreateSimulatedClick(node));
  EventDispatcher(node, *click).Dispatch();
  nodes.erase(std::find(nodes.begin(), nodes.end(), &node));
}

DispatchEventResult EventDispatcher::Dispatch() {
  // The DOM Standard's dispatch, in Blink's EventDispatcher::Dispatch().
  EventPath& path = *event_->GetEventPath();
  if (path.IsEmpty()) {
    return DispatchEventResult::kNotCanceled;
  }
  path.EnsureWindowEventContext();
  const bool is_click = event_->IsMouseEvent() && event_->Type() == u"click";
  // A click is the activation event, and Blink's textInput, which a Page
  // never fires.
  Node* const activation_target =
      is_click ? FindActivationTarget(*node_, *event_) : nullptr;
  event_->SetTarget(node_.Get());
  EventDispatchHandlingState* pre_dispatch_state = nullptr;
  if (DispatchEventLegacyPreActivationBehavior(
          activation_target, pre_dispatch_state) == Continuation::kContinue) {
    if (DispatchEventAtCapturing() == Continuation::kContinue) {
      DispatchEventAtBubbling();
    }
  }
  DispatchEventPostProcess(activation_target, pre_dispatch_state);
  return EventTarget::GetDispatchEventResult(*event_);
}

EventDispatcher::Continuation
EventDispatcher::DispatchEventLegacyPreActivationBehavior(
    Node* activation_target,
    EventDispatchHandlingState*& pre_dispatch_state) {
  if (activation_target) {
    pre_dispatch_state =
        activation_target->LegacyPreActivationBehavior(*event_);
  }
  return event_->GetEventPath()->IsEmpty() || event_->PropagationStopped()
             ? Continuation::kDone
             : Continuation::kContinue;
}

EventDispatcher::Continuation EventDispatcher::DispatchEventAtCapturing() {
  // The capturing listeners from the top down. At the target, the phase is
  // AT_TARGET and only its capturing listeners run. Blink skips the phase
  // when its document has no capturing listener at all, which changes
  // nothing that a page sees.
  event_->SetPhase(Event::Phase::kCapturing);
  const EventPath& path = *event_->GetEventPath();
  if (path.GetWindowEventContext().HandleLocalEvents(*event_) &&
      event_->PropagationStopped()) {
    return Continuation::kDone;
  }
  for (size_t i = path.size(); i > 0; --i) {
    const NodeEventContext& context = path.at(i - 1);
    if (context.CurrentTargetSameAsTarget()) {
      event_->SetPhase(Event::Phase::kAtTarget);
      event_->SetFireOnlyCaptureListenersAtTarget(true);
      context.HandleLocalEvents(*event_);
      event_->SetFireOnlyCaptureListenersAtTarget(false);
    } else {
      event_->SetPhase(Event::Phase::kCapturing);
      context.HandleLocalEvents(*event_);
    }
    if (event_->PropagationStopped()) {
      return Continuation::kDone;
    }
  }
  return Continuation::kContinue;
}

void EventDispatcher::DispatchEventAtBubbling() {
  // The other listeners from the target up: at the target, its listeners
  // that do not capture, and above it, for an event that bubbles, each
  // node's, then the window's.
  const EventPath& path = *event_->GetEventPath();
  for (size_t i = 0; i < path.size(); ++i) {
    const NodeEventContext& context = path.at(i);
    if (context.CurrentTargetSameAsTarget()) {
      event_->SetPhase(Event::Phase::kAtTarget);
      event_->SetFireOnlyNonCaptureListenersAtTarget(true);
      context.HandleLocalEvents(*event_);
      event_->SetFireOnlyNonCaptureListenersAtTarget(false);
    } else if (event_->Bubbles() && !event_->CancelBubble()) {
      event_->SetPhase(Event::Phase::kBubbling);
      context.HandleLocalEvents(*event_);
    } else {
      continue;
    }
    if (event_->PropagationStopped()) {
      return;
    }
  }
  if (event_->Bubbles() && !event_->CancelBubble()) {
    event_->SetPhase(Event::Phase::kBubbling);
    path.GetWindowEventContext().HandleLocalEvents(*event_);
  }
}

void EventDispatcher::DispatchEventPostProcess(
    Node* activation_target,
    EventDispatchHandlingState* pre_dispatch_state) {
  event_->SetTarget(node_.Get());
  event_->SetPhase(Event::Phase::kNone);
  event_->SetCurrentTarget(nullptr);
  event_->SetStopPropagation(false);
  event_->SetStopImmediatePropagation(false);

  const bool is_click = event_->IsMouseEvent() && event_->Type() == u"click";
  const bool clean_up_activation_behavior = IsActivationFeatureOn(
      bindings::RuntimeFeature::kCleanUpActivationBehavior);
  if (is_click) {
    // Listeners may have changed which nodes have activation behaviour, so
    // the target is found again, unless legacy-pre-activation behaviour ran,
    // whose activation behaviour has to commit or revert it.
    if (clean_up_activation_behavior && !pre_dispatch_state) {
      activation_target = FindActivationTarget(*node_, *event_);
    }
    if (activation_target) {
      activation_target->RunActivationBehaviorBeforeDOMActivate(
          *event_, pre_dispatch_state);
    }
  }

  // The DOM Events spec says that events dispatched by script, other than a
  // click, run no default handlers.
  const bool is_trusted_or_click = event_->IsTrusted() || is_click;
  if (!event_->DefaultPrevented() && !event_->DefaultHandled() &&
      is_trusted_or_click) {
    // Blink's: for each node of the path, in bubbling order, its default
    // event handler, then its activation behaviour if it is the activation
    // target or has activation behaviour, until the click is handled.
    const bool run_activation_behavior =
        clean_up_activation_behavior && is_click;
    auto is_handled = [this]() {
      return event_->DefaultHandled() || event_->DefaultPrevented();
    };
    auto run_activation_behavior_for = [&](Node& node) {
      if (!run_activation_behavior || is_handled()) {
        return;
      }
      if (&node == activation_target) {
        node.RunActivationBehavior(*event_, pre_dispatch_state);
        return;
      }
      if (!node.HasActivationBehavior()) {
        return;
      }
      node.RunActivationBehaviorBeforeDOMActivate(*event_, nullptr);
      if (!is_handled()) {
        node.RunActivationBehavior(*event_, nullptr);
      }
    };
    node_->DefaultEventHandler(*event_);
    run_activation_behavior_for(*node_);
    const EventPath& path = *event_->GetEventPath();
    if (!is_handled() && event_->Bubbles()) {
      for (size_t i = 1; i < path.size(); ++i) {
        Node& node = path.at(i).GetNode();
        node.DefaultEventHandler(*event_);
        run_activation_behavior_for(node);
        if (is_handled()) {
          break;
        }
      }
    }
  }

  // The DOM Standard's step 16: the target the window sees, which is null
  // for load, as Blink's WindowEventContext has none for it, and once shadow
  // trees come, for a target in one.
  event_->SetTarget(event_->GetEventPath()->GetWindowEventContext().Target());
}

}  // namespace mitto
