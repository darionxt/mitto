// The DOM's Event, which an EventTarget dispatches to its listeners.
//
// It follows Blink's Event: a node's dispatch gives it a path through the tree
// to the window (event_path.h), which composedPath() reads, and the dispatch
// (event_dispatcher.h) sets its phase, target and current target as it goes.
//
// Upstream: third_party/blink/renderer/core/dom/events/event.h
// Upstream: third_party/blink/renderer/core/dom/events/event.cc

#ifndef MITTO_DOM_EVENT_H_
#define MITTO_DOM_EVENT_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class EventPath;
class EventTarget;
class Node;

class Event : public bindings::ScriptWrappable {
 public:
  // The values of eventPhase.
  enum class Phase : uint16_t {
    kNone = 0,
    kCapturing = 1,
    kAtTarget = 2,
    kBubbling = 3,
  };

  // How the listener that runs treats preventDefault(), as Blink's
  // Event::PassiveMode: a listener that is passive by default, or that Blink
  // made passive for a document's scrolling, cancels nothing without saying
  // so, and one that said `passive: true` cancels nothing with a console error.
  enum class PassiveMode {
    kNotPassive,
    kNotPassiveDefault,
    kPassive,
    kPassiveDefault,
    kPassiveForcedDocumentLevel,
  };

  static Event* Create(const std::u16string& type,
                       const bindings::EventInit& init);

  Event(const std::u16string& type, const bindings::EventInit& init);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Type() const { return type_; }
  EventTarget* Target() const;
  EventTarget* CurrentTarget() const;
  std::vector<EventTarget*> ComposedPath() const;
  uint16_t EventPhase() const { return static_cast<uint16_t>(phase_); }
  void StopPropagation() { stop_propagation_ = true; }
  void StopImmediatePropagation();
  bool Bubbles() const { return bubbles_; }
  bool Cancelable() const { return cancelable_; }
  void PreventDefault();
  bool DefaultPrevented() const { return canceled_; }
  bool Composed() const { return composed_; }
  bool IsTrusted() const { return trusted_; }
  double TimeStamp() const;
  void InitEvent(const std::u16string& type, bool bubbles, bool cancelable);
  EventTarget* SrcElement() const { return Target(); }
  bool LegacyReturnValue() const { return !canceled_; }
  void SetLegacyReturnValue(bool value);
  bool CancelBubble() const { return stop_propagation_; }
  void SetCancelBubble(bool value);

  // Whether its interface is MouseEvent or one that inherits from it, such as
  // PointerEvent, whose click is an activation event.
  virtual bool IsMouseEvent() const;
  // Whether it is an ErrorEvent, whose error at a window an `onerror`
  // handler takes as five arguments.
  virtual bool IsErrorEvent() const;
  // Whether it is a BeforeUnloadEvent, whose returnValue an `onbeforeunload`
  // handler sets.
  virtual bool IsBeforeUnloadEvent() const;

  // What a dispatch reads and sets.
  bool IsBeingDispatched() const { return phase_ != Phase::kNone; }
  bool PropagationStopped() const { return stop_propagation_; }
  bool ImmediatePropagationStopped() const {
    return stop_immediate_propagation_;
  }
  void SetStopPropagation(bool stop) { stop_propagation_ = stop; }
  void SetStopImmediatePropagation(bool stop) {
    stop_immediate_propagation_ = stop;
  }
  void SetTrusted(bool trusted) { trusted_ = trusted; }
  void SetComposed(bool composed) { composed_ = composed; }
  void SetHandlingPassive(PassiveMode mode) { handling_passive_ = mode; }
  void SetTarget(EventTarget* target);
  void SetCurrentTarget(EventTarget* target);
  void SetPhase(Phase phase) { phase_ = phase; }
  // At the target, a dispatch runs its capturing listeners while it captures,
  // and the others while it bubbles, as Blink's EventDispatcher does.
  bool FireOnlyCaptureListenersAtTarget() const {
    return fire_only_capture_listeners_at_target_;
  }
  void SetFireOnlyCaptureListenersAtTarget(bool value) {
    fire_only_capture_listeners_at_target_ = value;
  }
  bool FireOnlyNonCaptureListenersAtTarget() const {
    return fire_only_non_capture_listeners_at_target_;
  }
  void SetFireOnlyNonCaptureListenersAtTarget(bool value) {
    fire_only_non_capture_listeners_at_target_ = value;
  }
  // Whether a default event handler handled it, as DOMActivate that a
  // listener canceled marks its click, which then runs no activation
  // behaviour.
  bool DefaultHandled() const { return default_handled_; }
  void SetDefaultHandled() { default_handled_ = true; }
  // The DOM Standard's "in shadow tree" of the listener that runs, which
  // decides whether window.event is the event. Always false until shadow
  // trees come.
  bool InvocationTargetInShadowTree() const {
    return invocation_target_in_shadow_tree_;
  }
  void SetInvocationTargetInShadowTree(bool value) {
    invocation_target_in_shadow_tree_ = value;
  }
  // The DOM Standard's legacyOutputDidListenersThrowFlag: whether a listener
  // threw while the event was dispatched, which IndexedDB reads to abort the
  // request's transaction, as Blink's JSBasedEventListener::Invoke() sets it.
  bool LegacyDidListenersThrow() const { return legacy_did_listeners_throw_; }
  void LegacySetDidListenersThrowFlag() { legacy_did_listeners_throw_ = true; }

  // The event that this one stands for, as a click's DOMActivate does.
  void SetUnderlyingEvent(const Event* event);
  const Event* UnderlyingEvent() const { return underlying_event_.Get(); }
  // Takes the underlying event's path at the next dispatch, as DOMActivate
  // does at a node that its click's listeners took out of the document.
  void SetCopyEventPathFromUnderlyingEvent() {
    copy_event_path_from_underlying_event_ = true;
  }

  // Works out the event's path from `node`, for a dispatch there.
  void InitEventPath(Node& node);
  EventPath* GetEventPath() const { return event_path_.Get(); }

 private:
  std::u16string type_;
  bool bubbles_;
  bool cancelable_;
  bool composed_;
  bool trusted_ = false;
  bool stop_propagation_ = false;
  bool stop_immediate_propagation_ = false;
  bool canceled_ = false;
  bool default_handled_ = false;
  bool fire_only_capture_listeners_at_target_ = false;
  bool fire_only_non_capture_listeners_at_target_ = false;
  bool invocation_target_in_shadow_tree_ = false;
  bool copy_event_path_from_underlying_event_ = false;
  bool legacy_did_listeners_throw_ = false;
  PassiveMode handling_passive_ = PassiveMode::kNotPassiveDefault;
  Phase phase_ = Phase::kNone;
  const base::TimeTicks created_;
  cppgc::Member<EventTarget> target_;
  cppgc::Member<EventTarget> current_target_;
  cppgc::Member<const Event> underlying_event_;
  cppgc::Member<EventPath> event_path_;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_H_
