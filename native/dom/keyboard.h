// The Keyboard Map API's Keyboard, navigator.keyboard: getLayoutMap(), as
// Chrome's browser answers it for a top-level page. Locking the keyboard is a
// Known Gap.
//
// Upstream: third_party/blink/renderer/modules/keyboard/keyboard.cc
// Upstream: third_party/blink/renderer/modules/keyboard/keyboard_layout.cc

#ifndef MITTO_DOM_KEYBOARD_H_
#define MITTO_DOM_KEYBOARD_H_

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class Keyboard final : public bindings::ScriptWrappable {
 public:
  static Keyboard* Create();

  Keyboard();
  ~Keyboard() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The calls before the browser answers share
  // one promise, which resolves from a task of its own with a new map, and a
  // later call asks again.
  v8::Local<v8::Promise> GetLayoutMap(
      bindings::ExceptionState& exception_state);

 private:
  v8::TracedReference<v8::Promise> pending_;
};

}  // namespace mitto

#endif  // MITTO_DOM_KEYBOARD_H_
