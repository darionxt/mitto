#include "mitto/dom/notification.h"

#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_notification.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// Whether the browser asks about notifications of the current context, where
// NotificationPermissionContext denies them to one that is not secure, and to a
// frame of another origin than the top-level one's, as an opaque one is, and
// NotificationManager to one that has gone.
bool MayAsk() {
  const ExecutionContext& context = ExecutionContext::Current();
  return context.HasContext() && context.IsSecureContextForBindings() &&
         !(context.IsWindow() && Window::Current().IsCrossOriginToTop());
}

}  // namespace

// static
Notification* Notification::Create() {
  return bindings::MakeGarbageCollected<Notification>();
}

// static
Notification* Notification::Create(const std::u16string& title,
                                   const bindings::NotificationOptions& options,
                                   bindings::ExceptionState& exception_state) {
  // Notification::Create(). NotificationConstructor is on for a desktop, but
  // a Page's set can turn it off. A Page has no service worker.
  if (!bindings::PerIsolateData::From(exception_state.GetIsolate())
           ->features()
           .IsEnabled(bindings::RuntimeFeature::kNotificationConstructor)) {
    exception_state.ThrowTypeError(
        "Illegal constructor. Use ServiceWorkerRegistration.showNotification() "
        "instead.");
    return nullptr;
  }
  if (!options.actions.empty()) {
    exception_state.ThrowTypeError(
        "Actions are only supported for persistent notifications shown using "
        "ServiceWorkerRegistration.showNotification().");
    return nullptr;
  }
  // The dictionary reads showTrigger only with NotificationTriggers on.
  if (options.show_trigger) {
    exception_state.ThrowTypeError(
        "ShowTrigger is only supported for persistent notifications shown "
        "using ServiceWorkerRegistration.showNotification().");
    return nullptr;
  }
  // CreateNotificationData().
  if (options.vibrate && options.silent) {
    exception_state.ThrowTypeError(
        "Silent notifications must not specify vibration patterns.");
    return nullptr;
  }
  if (options.renotify && options.tag.empty()) {
    exception_state.ThrowTypeError(
        "Notifications which set the renotify flag must specify a non-empty "
        "tag.");
    return nullptr;
  }
  if (!options.data.IsEmpty() && !options.data->IsNull()) {
    // The serializer throws its DataCloneError on the isolate.
    const bindings::TryRethrowScope rethrow_scope(exception_state.GetIsolate(),
                                                  exception_state);
    if (!SerializedScriptValue::SerializeForStorage(
            exception_state.GetIsolate(), options.data)) {
      return nullptr;
    }
  }
  // A context that has gone throws, as one whose frame a getter of `data`
  // removed, or that of a frame removed before.
  v8::Isolate* const isolate = exception_state.GetIsolate();
  ExecutionContext* const context =
      ExecutionContext::From(isolate, isolate->GetCurrentContext());
  if (!context || !context->HasContext()) {
    exception_state.ThrowTypeError("Illegal invocation.");
    return nullptr;
  }
  Notification* const notification = Create();
  notification->keep_alive_ = notification;
  context->AddContextDestroyedCallback(base::BindOnce(
      [](cppgc::WeakPersistent<Notification> notification) {
        if (notification) {
          notification->keep_alive_.Clear();
        }
      },
      cppgc::WeakPersistent<Notification>(notification)));
  // SchedulePrepareShow() and PrepareShow(): a Page has no permission to show
  // it, so error fires from a task of its own.
  PostTaskInContext(context, TaskType::kMiscPlatformAPI,
                    base::BindOnce(
                        [](cppgc::Persistent<Notification> notification) {
                          Event* const event =
                              Event::Create(u"error", bindings::EventInit());
                          event->SetTrusted(true);
                          notification->DispatchEvent(event);
                        },
                        cppgc::Persistent<Notification>(notification)));
  return notification;
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
  // browser, whose answer elsewhere is the content setting's default, ask,
  // which a context with no frame, as a worker's, also reads as "denied", as a
  // frame of another origin than the top-level one's does.
  return MayAsk() && ExecutionContext::Current().IsWindow() ? u"default"
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
  if (MayAsk()) {
    return resolver->GetPromise();
  }
  // The browser's permission context denies the request before any prompt, and
  // NotificationManager calls the deprecated callback with the answer before it
  // resolves.
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
            // The callback runs in its own context, if that is there still.
            v8::Local<v8::Context> context;
            if (!callback.IsEmpty() &&
                callback.Get(isolate)->GetCreationContext(isolate).ToLocal(
                    &context)) {
              const ExecutionContext* const owner =
                  ExecutionContext::From(isolate, context);
              if (!owner || !owner->HasContext()) {
                return denied;
              }
              // The window that asked stays the incumbent one, as Blink's
              // CallbackInvokeHelper keeps it.
              const v8::Context::BackupIncumbentScope incumbent_scope(
                  isolate->GetCurrentContext());
              const v8::Context::Scope context_scope(context);
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
                bindings::ReportException(isolate, try_catch.Exception(),
                                          try_catch.Message());
              }
            }
            return denied;
          },
          std::move(callback)));
  return resolver->GetPromise();
}

}  // namespace mitto
