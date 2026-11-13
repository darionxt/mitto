#include "mitto/dom/geolocation.h"

#include <string>
#include <tuple>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_geolocation.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/geolocation_position_error.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace {

// SecurityOrigin::IsSecure()'s message for a page that is not a secure
// context, which Blink's Geolocation passes on.
constexpr char16_t kInsecureOrigin[] =
    u"Only secure origins are allowed (see: https://goo.gl/Y0ZkNV).";
constexpr char16_t kTimeoutExpired[] = u"Timeout expired";

}  // namespace

// One request of getCurrentPosition() or watchPosition(), as Blink's
// GeoNotifier: its callbacks, and the timer of its timeout or of an error
// that it reports from a task of its own.
class GeoNotifier final : public cppgc::GarbageCollected<GeoNotifier> {
 public:
  GeoNotifier(v8::Isolate* isolate,
              Geolocation* geolocation,
              v8::Local<v8::Function> success_callback,
              v8::Local<v8::Function> error_callback,
              uint32_t timeout_ms,
              int32_t watch_id)
      : geolocation_(geolocation),
        success_callback_(isolate, success_callback),
        timeout_ms_(timeout_ms),
        watch_id_(watch_id) {
    if (!error_callback.IsEmpty()) {
      error_callback_.Reset(isolate, error_callback);
    }
  }

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(geolocation_);
    visitor->Trace(success_callback_);
    visitor->Trace(error_callback_);
    visitor->Trace(next_);
  }

  // Blink's GeoNotifier::SetFatalError(): the error goes to the error
  // callback from a task of its own, and ends the request.
  void SetFatalError(uint16_t code, std::u16string message) {
    fatal_code_ = code;
    fatal_message_ = std::move(message);
    Start(base::TimeDelta());
  }
  // Blink's GeoNotifier::StartTimer(): the request's timeout.
  void StartTimer() { Start(base::Milliseconds(timeout_ms_)); }
  void Stop() { timer_.Stop(); }

  uint16_t fatal_code() const { return fatal_code_; }
  const std::u16string& fatal_message() const { return fatal_message_; }
  int32_t watch_id() const { return watch_id_; }
  v8::Local<v8::Function> success_callback(v8::Isolate* isolate) const {
    return success_callback_.Get(isolate);
  }
  v8::Local<v8::Function> error_callback(v8::Isolate* isolate) const {
    return error_callback_.Get(isolate);
  }
  GeoNotifier* next() const { return next_.Get(); }
  void set_next(GeoNotifier* next) { next_ = next; }

 private:
  void Start(base::TimeDelta delay) {
    timer_.Start(
        FROM_HERE, delay,
        base::BindOnce(&Geolocation::Fire, base::Unretained(geolocation_.Get()),
                       base::Unretained(this)));
  }

  cppgc::Member<Geolocation> geolocation_;
  v8::TracedReference<v8::Function> success_callback_;
  v8::TracedReference<v8::Function> error_callback_;
  const uint32_t timeout_ms_;
  // 0 for getCurrentPosition().
  const int32_t watch_id_;
  uint16_t fatal_code_ = 0;
  std::u16string fatal_message_;
  base::OneShotTimer timer_;
  cppgc::Member<GeoNotifier> next_;
};

// static
Geolocation* Geolocation::Create() {
  return bindings::MakeGarbageCollected<Geolocation>();
}

Geolocation::Geolocation() = default;
Geolocation::~Geolocation() = default;

const bindings::WrapperTypeInfo* Geolocation::GetWrapperTypeInfo() const {
  return &bindings::V8Geolocation::kWrapperTypeInfo;
}

void Geolocation::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(first_);
}

void Geolocation::GetCurrentPositionForBindings(
    v8::Local<v8::Function> success_callback,
    v8::Local<v8::Function> error_callback,
    const bindings::PositionOptions& options) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  StartRequest(bindings::MakeGarbageCollected<GeoNotifier>(
      isolate, this, success_callback, error_callback, options.timeout,
      /*watch_id=*/0));
}

int32_t Geolocation::WatchPositionForBindings(
    v8::Local<v8::Function> success_callback,
    v8::Local<v8::Function> error_callback,
    const bindings::PositionOptions& options) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // ExecutionContext::CircularSequentialID(): from 1, never 0.
  last_watch_id_ = last_watch_id_ == INT32_MAX ? 1 : last_watch_id_ + 1;
  StartRequest(bindings::MakeGarbageCollected<GeoNotifier>(
      isolate, this, success_callback, error_callback, options.timeout,
      last_watch_id_));
  return last_watch_id_;
}

void Geolocation::ClearWatch(int32_t watch_id) {
  for (GeoNotifier* notifier = first_.Get(); notifier;
       notifier = notifier->next()) {
    if (watch_id != 0 && notifier->watch_id() == watch_id) {
      Remove(notifier);
      return;
    }
  }
}

void Geolocation::StartRequest(GeoNotifier* notifier) {
  notifier->set_next(first_.Get());
  first_ = notifier;
  if (!Window::Current().IsSecureContextForBindings()) {
    notifier->SetFatalError(GeolocationPositionError::kPermissionDenied,
                            kInsecureOrigin);
    return;
  }
  // A top-level page of a tab that a Session opened is visible, and has no
  // position cached. The browser shows its prompt, which nobody answers, so
  // only the timeout can end the request.
  notifier->StartTimer();
}

void Geolocation::Remove(GeoNotifier* notifier) {
  notifier->Stop();
  GeoNotifier* previous = nullptr;
  for (GeoNotifier* current = first_.Get(); current;
       current = current->next()) {
    if (current == notifier) {
      if (previous) {
        previous->set_next(current->next());
      } else {
        first_ = current->next();
      }
      return;
    }
    previous = current;
  }
}

void Geolocation::Fire(GeoNotifier* notifier) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  // Blink's GeoNotifier::TimerFired(): a fatal error ends any request, and a
  // timeout ends a one-shot, while a watch waits on. The callback runs in its
  // own context, which has to be there still.
  const bool fatal = notifier->fatal_code() != 0;
  const uint16_t code =
      fatal ? notifier->fatal_code() : GeolocationPositionError::kTimeout;
  const std::u16string message =
      fatal ? notifier->fatal_message() : std::u16string(kTimeoutExpired);
  const v8::Local<v8::Function> error_callback =
      notifier->error_callback(isolate);
  v8::Local<v8::Context> context;
  if (!notifier->success_callback(isolate)->GetCreationContext(isolate).ToLocal(
          &context)) {
    Remove(notifier);
    return;
  }
  const v8::Context::Scope context_scope(context);
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    Remove(notifier);
    return;
  }
  if (!error_callback.IsEmpty()) {
    v8::Local<v8::Value> argument =
        GeolocationPositionError::Create(code, message)->ToV8(isolate);
    // As Blink's V8ScriptRunner::CallFunction(): what the callback queued
    // runs once it returns.
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    const v8::TryCatch try_catch(isolate);
    if (error_callback->Call(context, v8::Undefined(isolate), 1, &argument)
            .IsEmpty() &&
        try_catch.HasCaught()) {
      bindings::ReportException(isolate, try_catch.Exception());
    }
  }
  if (fatal || notifier->watch_id() == 0) {
    Remove(notifier);
  }
}

}  // namespace mitto
