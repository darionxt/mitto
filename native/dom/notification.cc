#include "mitto/dom/notification.h"

#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_notification.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
Notification* Notification::Create() {
  return bindings::MakeGarbageCollected<Notification>();
}

// static
bindings::ScriptWrappable* Notification::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Notification>(
      info, bindings::V8Notification::kWrapperTypeInfo);
}

Notification::Notification() = default;
Notification::~Notification() = default;

const bindings::WrapperTypeInfo* Notification::GetWrapperTypeInfo() const {
  return &bindings::V8Notification::kWrapperTypeInfo;
}

// static
std::u16string Notification::Permission() {
  // Blink answers "denied" in a context that is not secure without asking the
  // browser, whose answer elsewhere is the content setting's default, ask.
  return Window::Current().IsSecureContextForBindings() ? u"default"
                                                        : u"denied";
}

// static
v8::Local<v8::Promise> Notification::RequestPermission(
    std::optional<v8::Local<v8::Function>> deprecated_callback) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  if (Window::Current().IsSecureContextForBindings()) {
    return resolver->GetPromise();
  }
  // A context that is not secure: the browser's permission context denies the
  // request before any prompt, and NotificationManager calls the deprecated
  // callback with the answer before it resolves.
  v8::Global<v8::Function> callback;
  if (deprecated_callback && !deprecated_callback->IsEmpty()) {
    callback.Reset(isolate, *deprecated_callback);
  }
  ResolveFromTaskWith(
      isolate, resolver,
      base::BindOnce(
          [](v8::Global<v8::Function> callback, v8::Isolate* isolate) {
            const v8::Local<v8::Value> denied =
                v8::String::NewFromUtf8Literal(isolate, "denied");
            if (!callback.IsEmpty()) {
              const v8::Local<v8::Context> context =
                  isolate->GetCurrentContext();
              // As Blink's V8ScriptRunner::CallFunction(): what the callback
              // queued runs once it returns.
              const v8::MicrotasksScope microtasks_scope(
                  context, v8::MicrotasksScope::kRunMicrotasks);
              const v8::TryCatch try_catch(isolate);
              v8::Local<v8::Value> argument = denied;
              if (callback.Get(isolate)
                      ->Call(context, v8::Undefined(isolate), 1, &argument)
                      .IsEmpty() &&
                  try_catch.HasCaught()) {
                bindings::ReportException(isolate, try_catch.Exception());
              }
            }
            return denied;
          },
          std::move(callback)));
  return resolver->GetPromise();
}

}  // namespace mitto
