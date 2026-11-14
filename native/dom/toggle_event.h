// The HTML Standard's ToggleEvent, which a details element fires once it
// opens or closes, with the state before and after.
//
// Upstream: third_party/blink/renderer/core/events/toggle_event.h
// Upstream: third_party/blink/renderer/core/events/toggle_event.cc

#ifndef MITTO_DOM_TOGGLE_EVENT_H_
#define MITTO_DOM_TOGGLE_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class Element;

class ToggleEvent final : public Event {
 public:
  static ToggleEvent* Create(const std::u16string& type,
                             const bindings::ToggleEventInit& init);
  // A trusted event that neither bubbles nor can be canceled, from
  // `old_state` to `new_state`, "open" or "closed", as Blink's details
  // element makes it.
  static ToggleEvent* CreateTrusted(const std::u16string& type,
                                    const std::u16string& old_state,
                                    const std::u16string& new_state);

  ToggleEvent(const std::u16string& type,
              const bindings::ToggleEventInit& init);
  ~ToggleEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& OldState() const { return old_state_; }
  const std::u16string& NewState() const { return new_state_; }
  // The element that caused it, which no shadow tree hides yet.
  Element* Source() const { return source_.Get(); }

 private:
  const std::u16string old_state_;
  const std::u16string new_state_;
  const cppgc::Member<Element> source_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TOGGLE_EVENT_H_
