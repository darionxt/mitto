// The HTML standard's PopStateEvent, which a window gets when its history
// moves. The Runtime has no history yet, so only a page's own events have one;
// it is here because one of its members is behind a runtime-enabled feature.
//
// Upstream: third_party/blink/renderer/core/events/pop_state_event.h
// Upstream: third_party/blink/renderer/core/events/pop_state_event.cc

#ifndef MITTO_DOM_POP_STATE_EVENT_H_
#define MITTO_DOM_POP_STATE_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class PopStateEvent final : public Event {
 public:
  static PopStateEvent* Create(const std::u16string& type,
                               const bindings::PopStateEventInit& init);

  PopStateEvent(const std::u16string& type,
                const bindings::PopStateEventInit& init);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> State(bindings::ExceptionState& exception_state) const;
  bool HasUAVisualTransition() const { return has_ua_visual_transition_; }

 private:
  v8::TracedReference<v8::Value> state_;
  const bool has_ua_visual_transition_;
};

}  // namespace mitto

#endif  // MITTO_DOM_POP_STATE_EVENT_H_
