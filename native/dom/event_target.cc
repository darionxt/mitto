#include "mitto/dom/event_target.h"

#include <variant>

#include "base/check.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_event_target.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The DOM standard's "flatten" of addEventListener()'s options.
struct FlatOptions {
  bool capture = false;
  bool passive = false;
  bool once = false;
};

FlatOptions Flatten(
    const std::optional<bindings::AddEventListenerOptionsOrBoolean>& options) {
  FlatOptions flat;
  if (!options) {
    return flat;
  }
  if (const bool* capture = std::get_if<bool>(&*options)) {
    flat.capture = *capture;
    return flat;
  }
  const auto& dictionary = std::get<bindings::AddEventListenerOptions>(*options);
  flat.capture = dictionary.capture;
  // Without `passive`, a listener is not passive. The DOM standard makes some
  // listeners of a window, a document or a body passive by default, which no
  // target of the Runtime is yet.
  flat.passive = dictionary.passive.value_or(false);
  flat.once = dictionary.once;
  return flat;
}

bool FlattenCapture(
    const std::optional<bindings::EventListenerOptionsOrBoolean>& options) {
  if (!options) {
    return false;
  }
  if (const bool* capture = std::get_if<bool>(&*options)) {
    return *capture;
  }
  return std::get<bindings::EventListenerOptions>(*options).capture;
}

// Calls a listener's callback, as Web IDL calls a user object's operation:
// the callback itself if it is a function, with the current target for
// `this`, and otherwise its handleEvent(), with the callback for `this`.
void CallListener(v8::Isolate* isolate,
                  v8::Local<v8::Object> callback,
                  v8::Local<v8::Value> current_target,
                  v8::Local<v8::Value> event) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Function> function;
  v8::Local<v8::Value> receiver = current_target;
  if (callback->IsFunction()) {
    function = callback.As<v8::Function>();
  } else {
    v8::Local<v8::Value> handle_event;
    if (!callback
             ->Get(context, v8::String::NewFromUtf8Literal(isolate, "handleEvent"))
             .ToLocal(&handle_event)) {
      bindings::ReportException(isolate, try_catch.Exception());
      return;
    }
    if (!handle_event->IsFunction()) {
      bindings::ThrowTypeError(isolate,
                               "Failed to execute 'handleEvent' on "
                               "'EventListener': The provided callback is not "
                               "callable.");
      bindings::ReportException(isolate, try_catch.Exception());
      return;
    }
    function = handle_event.As<v8::Function>();
    receiver = callback;
  }
  v8::Local<v8::Value> arguments[] = {event};
  if (function->Call(context, receiver, 1, arguments).IsEmpty() &&
      try_catch.HasCaught()) {
    bindings::ReportException(isolate, try_catch.Exception());
  }
}

}  // namespace

EventListenerEntry::EventListenerEntry(v8::Isolate* isolate,
                                       const std::u16string& type,
                                       v8::Local<v8::Object> callback,
                                       bool capture,
                                       bool passive,
                                       bool once)
    : type_(type),
      callback_(isolate, callback),
      capture_(capture),
      passive_(passive),
      once_(once) {}

EventListenerEntry::~EventListenerEntry() = default;

void EventListenerEntry::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(callback_);
}

// static
EventTarget* EventTarget::Create() {
  return bindings::MakeGarbageCollected<EventTarget>();
}

EventTarget::EventTarget() = default;
EventTarget::~EventTarget() = default;

const bindings::WrapperTypeInfo* EventTarget::GetWrapperTypeInfo() const {
  return &bindings::V8EventTarget::kWrapperTypeInfo;
}

void EventTarget::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  for (const cppgc::Member<EventListenerEntry>& entry : listeners_) {
    visitor->Trace(entry);
  }
}

void EventTarget::AddEventListener(
    const std::u16string& type,
    v8::Local<v8::Object> listener,
    const std::optional<bindings::AddEventListenerOptionsOrBoolean>& options) {
  const FlatOptions flat = Flatten(options);
  if (listener.IsEmpty()) {
    return;
  }
  for (const cppgc::Member<EventListenerEntry>& entry : listeners_) {
    if (entry->type() == type && entry->capture() == flat.capture &&
        entry->callback(v8::Isolate::GetCurrent())->StrictEquals(listener)) {
      return;
    }
  }
  listeners_.push_back(bindings::MakeGarbageCollected<EventListenerEntry>(
      v8::Isolate::GetCurrent(), type, listener, flat.capture, flat.passive,
      flat.once));
}

void EventTarget::RemoveEventListener(
    const std::u16string& type,
    v8::Local<v8::Object> listener,
    const std::optional<bindings::EventListenerOptionsOrBoolean>& options) {
  const bool capture = FlattenCapture(options);
  if (listener.IsEmpty()) {
    return;
  }
  for (const cppgc::Member<EventListenerEntry>& entry : listeners_) {
    if (entry->type() == type && entry->capture() == capture &&
        entry->callback(v8::Isolate::GetCurrent())->StrictEquals(listener)) {
      RemoveEntry(entry.Get());
      return;
    }
  }
}

void EventTarget::RemoveEntry(EventListenerEntry* entry) {
  entry->set_removed();
  std::erase_if(listeners_, [entry](const cppgc::Member<EventListenerEntry>& m) {
    return m.Get() == entry;
  });
}

bool EventTarget::DispatchEventForBindings(
    Event* event,
    bindings::ExceptionState& exception_state) {
  if (event->IsBeingDispatched()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The event is already being dispatched.");
    return false;
  }
  event->SetTrusted(false);
  return DispatchEvent(event);
}

bool EventTarget::DispatchEvent(Event* event) {
  // Blink's EventTarget::DispatchEventInternal().
  event->SetTarget(this);
  event->SetCurrentTarget(this);
  event->SetPhase(Event::Phase::kAtTarget);
  FireEventListeners(event);
  if (bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
          ->features()
          .IsEnabled(
              bindings::RuntimeFeature::kClearCurrentTargetAfterDispatch)) {
    event->SetCurrentTarget(nullptr);
  }
  event->SetPhase(Event::Phase::kNone);
  return !event->DefaultPrevented();
}

void EventTarget::FireEventListeners(Event* event) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // A copy, so that listeners that a listener adds wait for the next event,
  // and Persistent, so that the collector keeps them while scripts run.
  std::vector<cppgc::Persistent<EventListenerEntry>> entries(
      listeners_.begin(), listeners_.end());
  const v8::Local<v8::Value> current_target = ToV8(isolate);
  const v8::Local<v8::Value> event_wrapper = event->ToV8(isolate);
  for (const cppgc::Persistent<EventListenerEntry>& entry : entries) {
    if (entry->removed() || entry->type() != event->Type()) {
      continue;
    }
    if (event->ImmediatePropagationStopped()) {
      break;
    }
    if (entry->once()) {
      RemoveEntry(entry.Get());
    }
    event->SetInPassiveListener(entry->passive());
    CallListener(isolate, entry->callback(isolate), current_target,
                 event_wrapper);
    event->SetInPassiveListener(false);
  }
}

}  // namespace mitto
