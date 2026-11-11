// The DOM's Event, which an EventTarget dispatches to its listeners.
//
// It follows Blink's Event, for what dispatch on a single target needs: no
// event path beyond the target yet, so no capturing or bubbling through a tree.
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

class EventTarget;

class Event : public bindings::ScriptWrappable {
 public:
  // The values of eventPhase.
  enum class Phase : uint16_t {
    kNone = 0,
    kCapturing = 1,
    kAtTarget = 2,
    kBubbling = 3,
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

  // What EventTarget's dispatch reads and sets.
  bool IsBeingDispatched() const { return phase_ != Phase::kNone; }
  bool PropagationStopped() const { return stop_propagation_; }
  bool ImmediatePropagationStopped() const {
    return stop_immediate_propagation_;
  }
  void SetTrusted(bool trusted) { trusted_ = trusted; }
  void SetInPassiveListener(bool in_passive_listener) {
    in_passive_listener_ = in_passive_listener;
  }
  void SetTarget(EventTarget* target);
  void SetCurrentTarget(EventTarget* target);
  void SetPhase(Phase phase) { phase_ = phase; }

 private:
  std::u16string type_;
  bool bubbles_;
  bool cancelable_;
  bool composed_;
  bool trusted_ = false;
  bool stop_propagation_ = false;
  bool stop_immediate_propagation_ = false;
  bool canceled_ = false;
  bool in_passive_listener_ = false;
  Phase phase_ = Phase::kNone;
  const base::TimeTicks created_;
  cppgc::Member<EventTarget> target_;
  cppgc::Member<EventTarget> current_target_;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_H_
