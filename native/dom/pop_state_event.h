// The HTML standard's PopStateEvent, which a window gets when its history
// moves, with the entry's state, and a page makes with a state of its own.
//
// Upstream: third_party/blink/renderer/core/events/pop_state_event.h
// Upstream: third_party/blink/renderer/core/events/pop_state_event.cc

#ifndef MITTO_DOM_POP_STATE_EVENT_H_
#define MITTO_DOM_POP_STATE_EVENT_H_

#include <memory>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/event.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class History;

class PopStateEvent final : public Event {
 public:
  static PopStateEvent* Create(const std::u16string& type,
                               const bindings::PopStateEventInit& init);
  // The trusted popstate of `history`'s window, with `state`, none for null:
  // it does not bubble and cannot be canceled.
  static PopStateEvent* CreateForHistory(
      std::shared_ptr<const SerializedScriptValue> state,
      History* history);

  PopStateEvent(const std::u16string& type,
                const bindings::PopStateEventInit& init);
  PopStateEvent(std::shared_ptr<const SerializedScriptValue> state,
                History* history);
  ~PopStateEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The state of an event that history fires
  // is what history.state gives while the entry keeps that state, and its
  // own copy otherwise, read once, as Blink caches it.
  v8::Local<v8::Value> State(bindings::ExceptionState& exception_state);
  bool HasUAVisualTransition() const { return has_ua_visual_transition_; }

 private:
  // A page's own event's state, and then the one that history's has given.
  v8::TracedReference<v8::Value> state_;
  std::shared_ptr<const SerializedScriptValue> serialized_state_;
  cppgc::Member<History> history_;
  const bool has_ua_visual_transition_;
};

}  // namespace mitto

#endif  // MITTO_DOM_POP_STATE_EVENT_H_
