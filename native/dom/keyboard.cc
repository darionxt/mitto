#include "mitto/dom/keyboard.h"

#include "base/functional/bind.h"
#include "mitto/bindings/v8_keyboard.h"
#include "mitto/dom/keyboard_layout_map.h"
#include "mitto/dom/script_values.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
Keyboard* Keyboard::Create() {
  return bindings::MakeGarbageCollected<Keyboard>();
}

Keyboard::Keyboard() = default;
Keyboard::~Keyboard() = default;

const bindings::WrapperTypeInfo* Keyboard::GetWrapperTypeInfo() const {
  return &bindings::V8Keyboard::kWrapperTypeInfo;
}

void Keyboard::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(pending_);
}

v8::Local<v8::Promise> Keyboard::GetLayoutMap(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!pending_.IsEmpty()) {
    return pending_.Get(isolate);
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // KeyboardLayout::GotKeyboardLayoutMap(): the browser's map, after which
  // the next call asks again.
  ResolveFromTaskWith(isolate, resolver,
                      base::BindOnce(
                          [](cppgc::WeakPersistent<Keyboard> keyboard,
                             v8::Isolate* isolate) -> v8::Local<v8::Value> {
                            if (keyboard) {
                              keyboard->pending_.Reset();
                            }
                            return KeyboardLayoutMap::Create()->ToV8(isolate);
                          },
                          cppgc::WeakPersistent<Keyboard>(this)));
  pending_.Reset(isolate, resolver->GetPromise());
  return resolver->GetPromise();
}

}  // namespace mitto
