// The HTML Standard's BeforeUnloadEvent, which a window fires before a
// navigation unloads its document, and whose cancellation or returnValue asks
// the user to stay, which Chrome does only for a page that the user has
// activated. A page cannot make one.
//
// Upstream: third_party/blink/renderer/core/events/before_unload_event.h

#ifndef MITTO_DOM_BEFORE_UNLOAD_EVENT_H_
#define MITTO_DOM_BEFORE_UNLOAD_EVENT_H_

#include <string>

#include "mitto/dom/event.h"

namespace mitto {

class BeforeUnloadEvent final : public Event {
 public:
  // A trusted beforeunload that can be canceled and does not bubble, as
  // Blink's frame loader makes it.
  static BeforeUnloadEvent* Create();

  BeforeUnloadEvent();
  ~BeforeUnloadEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  bool IsBeforeUnloadEvent() const override;

  // The members of the interface.
  const std::u16string& ReturnValue() const { return return_value_; }
  void SetReturnValue(const std::u16string& value) { return_value_ = value; }

 private:
  std::u16string return_value_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BEFORE_UNLOAD_EVENT_H_
