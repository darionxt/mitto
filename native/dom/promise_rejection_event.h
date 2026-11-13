// The HTML Standard's PromiseRejectionEvent, which a window fires for a
// promise that a script rejected with no handler, unhandledrejection, and once
// one comes later, rejectionhandled, as Blink's PromiseRejectionEvent holds
// it.
//
// Upstream: third_party/blink/renderer/core/events/promise_rejection_event.h
// Upstream: third_party/blink/renderer/core/events/promise_rejection_event.cc

#ifndef MITTO_DOM_PROMISE_REJECTION_EVENT_H_
#define MITTO_DOM_PROMISE_REJECTION_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class PromiseRejectionEvent final : public Event {
 public:
  static PromiseRejectionEvent* Create(
      const std::u16string& type,
      const bindings::PromiseRejectionEventInit& init);
  // The event of a rejection that the Runtime reports: unhandledrejection,
  // which a listener can cancel, or rejectionhandled, which it cannot.
  static PromiseRejectionEvent* CreateForRejection(
      const std::u16string& type,
      bool cancelable,
      v8::Local<v8::Promise> promise,
      v8::Local<v8::Value> reason);

  PromiseRejectionEvent(const std::u16string& type,
                        const bindings::PromiseRejectionEventInit& init);
  ~PromiseRejectionEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Object> Promise() const;
  v8::Local<v8::Value> Reason() const;

 private:
  v8::TracedReference<v8::Promise> promise_;
  // Empty for an init without a reason, which reads as undefined.
  v8::TracedReference<v8::Value> reason_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PROMISE_REJECTION_EVENT_H_
