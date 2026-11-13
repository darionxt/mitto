// The DOM's CustomEvent, an event of the page's own with a `detail`.
//
// Upstream: third_party/blink/renderer/core/dom/events/custom_event.h
// Upstream: third_party/blink/renderer/core/dom/events/custom_event.cc

#ifndef MITTO_DOM_CUSTOM_EVENT_H_
#define MITTO_DOM_CUSTOM_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class CustomEvent final : public Event {
 public:
  static CustomEvent* Create(const std::u16string& type,
                             const bindings::CustomEventInit& init);

  CustomEvent(const std::u16string& type,
              const bindings::CustomEventInit& init);
  ~CustomEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Detail() const;
  void InitCustomEvent(const std::u16string& type,
                       bool bubbles,
                       bool cancelable,
                       v8::Local<v8::Value> detail);

 private:
  // Empty for null, as Blink keeps none for a detail that is null or
  // undefined.
  v8::TracedReference<v8::Value> detail_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CUSTOM_EVENT_H_
