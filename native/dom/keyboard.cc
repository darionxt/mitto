#include "mitto/dom/keyboard.h"

#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/per_context_data.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_keyboard.h"
#include "mitto/dom/document.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/keyboard_layout_map.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
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
  if (!ExecutionContext::OfReceiver().HasContext()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "Current frame is detached.");
    return v8::Local<v8::Promise>();
  }
  // The keyboard's own document asks, whose promise it is.
  const v8::Context::Scope receiver_scope(
      bindings::ReceiverRealmScope::CreationContext(isolate));
  if (!pending_.IsEmpty()) {
    return pending_.Get(isolate);
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // KeyboardLayout::GotKeyboardLayoutMap(): the browser's map, or that a
  // frame's permissions policy withholds it, after which the next call asks
  // again.
  Window& window = Window::Current();
  const bool denied = window.IsInFrame() &&
                      !window.GetDocument().IsFeatureEnabled("keyboard-map");
  auto answer = base::BindOnce(
      [](cppgc::WeakPersistent<Keyboard> keyboard, bool denied,
         v8::Isolate* isolate) -> v8::Local<v8::Value> {
        if (keyboard) {
          keyboard->pending_.Reset();
        }
        if (denied) {
          return bindings::PerIsolateData::From(isolate)
              ->dom_exception_hooks()
              .create(isolate, "SecurityError",
                      "getLayoutMap() must be called from a top-level browsing "
                      "context or allowed by the permission policy.");
        }
        return KeyboardLayoutMap::Create()->ToV8(isolate);
      },
      cppgc::WeakPersistent<Keyboard>(this), denied);
  if (denied) {
    RejectFromTaskWith(isolate, resolver, std::move(answer));
  } else {
    ResolveFromTaskWith(isolate, resolver, std::move(answer));
  }
  pending_.Reset(isolate, resolver->GetPromise());
  return resolver->GetPromise();
}

}  // namespace mitto
