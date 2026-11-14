#include "mitto/dom/event_target.h"

#include <tuple>
#include <variant>

#include "base/check.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_event_target.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/before_unload_event.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/node.h"
#include "mitto/dom/permissions_policy.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The DOM standard's "flatten" of addEventListener()'s options.
struct FlatOptions {
  bool capture = false;
  std::optional<bool> passive;
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
  const auto& dictionary =
      std::get<bindings::AddEventListenerOptions>(*options);
  flat.capture = dictionary.capture;
  flat.passive = dictionary.passive;
  flat.once = dictionary.once;
  return flat;
}

bool IsTouchScrollBlockingEvent(std::u16string_view type) {
  return type == u"touchstart" || type == u"touchmove";
}

bool IsWheelScrollBlockingEvent(std::u16string_view type) {
  return type == u"mousewheel" || type == u"wheel";
}

// Blink's EventPassiveMode().
Event::PassiveMode PassiveModeOf(const EventListenerEntry& entry) {
  if (!entry.passive()) {
    return entry.passive_specified() ? Event::PassiveMode::kNotPassive
                                     : Event::PassiveMode::kNotPassiveDefault;
  }
  if (entry.passive_forced_for_document_target()) {
    return Event::PassiveMode::kPassiveForcedDocumentLevel;
  }
  return entry.passive_specified() ? Event::PassiveMode::kPassive
                                   : Event::PassiveMode::kPassiveDefault;
}

// Blink's RegisteredEventListener::ShouldFire(): at the target, the capturing
// listeners while the dispatch captures and the others while it bubbles, and
// elsewhere those of the phase. A target that is not a node runs all of them
// at its target phase.
bool ShouldFire(const EventListenerEntry& entry, const Event& event) {
  if (event.FireOnlyCaptureListenersAtTarget()) {
    return entry.capture();
  }
  if (event.FireOnlyNonCaptureListenersAtTarget()) {
    return !entry.capture();
  }
  switch (static_cast<Event::Phase>(event.EventPhase())) {
    case Event::Phase::kCapturing:
      return entry.capture();
    case Event::Phase::kBubbling:
      return !entry.capture();
    default:
      return true;
  }
}

// The signal of addEventListener()'s options, if they have one.
AbortSignal* SignalOf(
    const std::optional<bindings::AddEventListenerOptionsOrBoolean>& options) {
  if (!options) {
    return nullptr;
  }
  const auto* dictionary =
      std::get_if<bindings::AddEventListenerOptions>(&*options);
  return dictionary && dictionary->signal ? *dictionary->signal : nullptr;
}

// The number that marks `callback` for the abort algorithms of the listeners
// that it is, kept on it in a private property, which a page cannot see, or
// 0 when it has none and `assign` is false. So an algorithm finds the
// listener without holding its function.
double ListenerIdOf(v8::Isolate* isolate,
                    v8::Local<v8::Object> callback,
                    bool assign) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Private> key = v8::Private::ForApi(
      isolate,
      v8::String::NewFromUtf8Literal(isolate, "mitto::EventTarget listener"));
  v8::Local<v8::Value> value;
  if (callback->GetPrivate(context, key).ToLocal(&value) && value->IsNumber()) {
    return value.As<v8::Number>()->Value();
  }
  if (!assign) {
    return 0;
  }
  static double last_id = 0;
  const double id = ++last_id;
  std::ignore =
      callback->SetPrivate(context, key, v8::Number::New(isolate, id));
  return id;
}

// The abort algorithm of a listener added with a signal: it removes the
// listener of its type, function and capture, as Blink's binds
// removeEventListener() with them, whichever registration that is by then,
// such as one that the page removed and added again without the signal.
// Blink holds the listener weakly, and its AbortSignalRegistry keeps the
// algorithm while the listener lives; this holds neither, and finds the
// function by its mark, so that it removes such a registration whenever a
// collection runs in between, and keeps no function alive.
class RemoveListenerAlgorithm final : public AbortSignal::Algorithm {
 public:
  RemoveListenerAlgorithm(EventTarget* target,
                          const std::u16string& type,
                          double listener_id,
                          bool capture)
      : target_(target),
        type_(type),
        listener_id_(listener_id),
        capture_(capture) {}

  void Run() override {
    if (EventTarget* const target = target_.Get()) {
      target->RemoveMarkedListener(type_, listener_id_, capture_);
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(target_);
  }

 private:
  const cppgc::WeakMember<EventTarget> target_;
  const std::u16string type_;
  const double listener_id_;
  const bool capture_;
};

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
  // The HTML Standard's clean up after running a callback: the microtasks that
  // a listener queues run once it returns, if no script is running, as
  // Blink's V8ScriptRunner::CallFunction() runs them. After the TryCatch, so
  // that what they throw is reported on its own.
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Function> function;
  v8::Local<v8::Value> receiver = current_target;
  if (callback->IsFunction()) {
    function = callback.As<v8::Function>();
  } else {
    v8::Local<v8::Value> handle_event;
    if (!callback
             ->Get(context,
                   v8::String::NewFromUtf8Literal(isolate, "handleEvent"))
             .ToLocal(&handle_event)) {
      if (try_catch.CanContinue()) {
        bindings::ReportException(isolate, try_catch.Exception(),
                                  try_catch.Message());
      }
      return;
    }
    if (!handle_event->IsFunction()) {
      bindings::ThrowTypeError(isolate,
                               "Failed to execute 'handleEvent' on "
                               "'EventListener': The provided callback is not "
                               "callable.");
      bindings::ReportException(isolate, try_catch.Exception(),
                                try_catch.Message());
      return;
    }
    function = handle_event.As<v8::Function>();
    receiver = callback;
  }
  v8::Local<v8::Value> arguments[] = {event};
  if (function->Call(context, receiver, 1, arguments).IsEmpty() &&
      try_catch.HasCaught() && try_catch.CanContinue()) {
    // Where it was thrown, as Blink's verbose TryCatch hands V8's message of
    // it to the window's error event.
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
}

// Blink's check in EventTarget::AddEventListenerInternal(): an unload listener
// that the permissions policy of the executing window's document does not
// allow, as where the seed deprecates unload, is refused, and the console
// reports the violation each time.
bool AllowsUnloadListener(const std::u16string& type) {
  if (type != u"unload") {
    return true;
  }
  Window& window = Window::Current();
  PermissionsPolicy* const policy = window.GetDocument().FeaturePolicy();
  if (!policy || policy->AllowsFeature(u"unload", std::nullopt)) {
    return true;
  }
  window.AddConsoleMessage(
      ConsoleLevel::kError,
      "Permissions policy violation: unload is not allowed in this document.");
  return false;
}

// Runs an event handler attribute's handler, as Blink's
// JSEventHandler::InvokeInternal() does: nothing for one that cannot be
// called, which a page can set; the current target for `this`; and for an
// ErrorEvent of type error at a window, the HTML Standard's special error
// event handling: five arguments, the event's message, filename, lineno, colno
// and error, and a return value of true cancels the event. For any other
// event, the event is its argument, and a return value of false cancels it,
// but for beforeunload, whose handler is an OnBeforeUnloadEventHandler: a
// return value that is not null or undefined, as a string, cancels a
// BeforeUnloadEvent and becomes its returnValue if that is empty.
void CallAttributeHandler(v8::Isolate* isolate,
                          v8::Local<v8::Object> handler,
                          EventTarget& current_target,
                          v8::Local<v8::Value> current_target_wrapper,
                          v8::Local<v8::Value> event_wrapper,
                          Event* event) {
  if (!handler->IsFunction()) {
    return;
  }
  const bool special_error_event_handling = event->IsErrorEvent() &&
                                            event->Type() == u"error" &&
                                            current_target.ToWindow();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> result;
  bool called = false;
  if (special_error_event_handling) {
    auto* const error_event = static_cast<ErrorEvent*>(event);
    v8::Local<v8::Value> arguments[] = {
        bindings::ToV8String(isolate, error_event->Message()),
        bindings::ToV8String(isolate, error_event->Filename()),
        v8::Integer::NewFromUnsigned(isolate, error_event->Lineno()),
        v8::Integer::NewFromUnsigned(isolate, error_event->Colno()),
        error_event->Error()};
    called = handler.As<v8::Function>()
                 ->Call(context, current_target_wrapper, 5, arguments)
                 .ToLocal(&result);
  } else {
    v8::Local<v8::Value> arguments[] = {event_wrapper};
    called = handler.As<v8::Function>()
                 ->Call(context, current_target_wrapper, 1, arguments)
                 .ToLocal(&result);
  }
  if (!called) {
    if (try_catch.HasCaught() && try_catch.CanContinue()) {
      bindings::ReportException(isolate, try_catch.Exception(),
                                try_catch.Message());
    }
    return;
  }
  if (result->IsNullOrUndefined()) {
    return;
  }
  if (event->Type() == u"beforeunload") {
    // The return value converted to DOMString?, as Web IDL converts a
    // callback's, which reports what the conversion throws.
    const v8::TryCatch conversion(isolate);
    std::u16string text;
    if (!bindings::FromV8String(isolate, result, &text)) {
      if (conversion.HasCaught() && conversion.CanContinue()) {
        bindings::ReportException(isolate, conversion.Exception(),
                                  conversion.Message());
      }
      return;
    }
    if (event->IsBeforeUnloadEvent()) {
      auto* const before_unload = static_cast<BeforeUnloadEvent*>(event);
      event->PreventDefault();
      if (before_unload->ReturnValue().empty()) {
        before_unload->SetReturnValue(text);
      }
    }
    return;
  }
  if (!result->IsBoolean()) {
    return;
  }
  const bool value = result.As<v8::Boolean>()->Value();
  if (special_error_event_handling ? value : !value) {
    event->PreventDefault();
  }
}

}  // namespace

EventListenerEntry::EventListenerEntry(v8::Isolate* isolate,
                                       const std::u16string& type,
                                       v8::Local<v8::Object> callback,
                                       bool capture,
                                       bool passive,
                                       bool once,
                                       bool attribute_handler,
                                       bool passive_specified,
                                       bool passive_forced_for_document_target)
    : type_(type),
      callback_(isolate, callback),
      capture_(capture),
      passive_(passive),
      once_(once),
      attribute_handler_(attribute_handler),
      passive_specified_(passive_specified),
      passive_forced_for_document_target_(passive_forced_for_document_target) {}

EventListenerEntry::~EventListenerEntry() = default;

void EventListenerEntry::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(callback_);
  visitor->Trace(next_);
  visitor->Trace(signal_);
}

void EventListenerEntry::set_signal(AbortSignal* signal) {
  signal_ = signal;
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
  visitor->Trace(first_listener_);
  visitor->Trace(last_listener_);
}

void EventTarget::AddEventListener(
    const std::u16string& type,
    v8::Local<v8::Object> listener,
    const std::optional<bindings::AddEventListenerOptionsOrBoolean>& options) {
  const FlatOptions flat = Flatten(options);
  if (listener.IsEmpty()) {
    return;
  }
  // Blink's SetDefaultAddEventListenerOptions(): a touch or wheel listener of
  // a window, a document, its root element or its body that does not say
  // `passive` is passive, so that it cannot block scrolling.
  bool passive = flat.passive.value_or(false);
  bool passive_forced = false;
  if (!flat.passive &&
      (IsTouchScrollBlockingEvent(type) || IsWheelScrollBlockingEvent(type)) &&
      IsTopLevelNode()) {
    passive = true;
    passive_forced = true;
  }
  // A signal that has aborted adds nothing, as in Blink before anything else.
  AbortSignal* const signal = SignalOf(options);
  if (signal && signal->Aborted()) {
    return;
  }
  if (!AllowsUnloadListener(type)) {
    return;
  }
  // Blink makes a target's EventTargetData here, before it checks for the same
  // listener, and keeps it after its listeners are removed.
  has_event_target_data_ = true;
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    // An event handler attribute's listener is the handler's own, never the
    // page's function, as Blink wraps the handler in a JSEventHandler.
    if (!entry->attribute_handler() && entry->type() == type &&
        entry->capture() == flat.capture &&
        entry->callback(v8::Isolate::GetCurrent())->StrictEquals(listener)) {
      return;
    }
  }
  EventListenerEntry* const entry =
      bindings::MakeGarbageCollected<EventListenerEntry>(
          v8::Isolate::GetCurrent(), type, listener, flat.capture, passive,
          flat.once, /*attribute_handler=*/false,
          /*passive_specified=*/flat.passive.has_value(), passive_forced);
  if (last_listener_) {
    last_listener_->set_next(entry);
  } else {
    first_listener_ = entry;
  }
  last_listener_ = entry;
  if (signal) {
    entry->set_signal(signal);
    signal->AddAlgorithm(
        bindings::MakeGarbageCollected<RemoveListenerAlgorithm>(
            this, type, ListenerIdOf(v8::Isolate::GetCurrent(), listener, true),
            flat.capture));
  }
  ListenersChanged(type);
}

void EventTarget::RemoveEventListener(
    const std::u16string& type,
    v8::Local<v8::Object> listener,
    const std::optional<bindings::EventListenerOptionsOrBoolean>& options) {
  const bool capture = FlattenCapture(options);
  if (listener.IsEmpty()) {
    return;
  }
  RemoveListener(type, listener, capture);
}

void EventTarget::RemoveListener(const std::u16string& type,
                                 v8::Local<v8::Object> callback,
                                 bool capture) {
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (!entry->attribute_handler() && entry->type() == type &&
        entry->capture() == capture &&
        entry->callback(v8::Isolate::GetCurrent())->StrictEquals(callback)) {
      RemoveEntry(entry);
      return;
    }
  }
}

void EventTarget::RemoveMarkedListener(const std::u16string& type,
                                       double listener_id,
                                       bool capture) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (!entry->attribute_handler() && entry->type() == type &&
        entry->capture() == capture &&
        ListenerIdOf(isolate, entry->callback(isolate), false) == listener_id) {
      RemoveEntry(entry);
      return;
    }
  }
}

bool EventTarget::HasEventListeners(std::u16string_view type) const {
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (entry->type() == type) {
      return true;
    }
  }
  return false;
}

void EventTarget::RemoveAllEventListeners() {
  // A dispatch that runs skips them from now on.
  while (EventListenerEntry* const entry = first_listener_.Get()) {
    RemoveEntry(entry);
  }
}

void EventTarget::RemoveEntry(EventListenerEntry* entry) {
  entry->set_removed();
  EventListenerEntry* previous = nullptr;
  for (EventListenerEntry* current = first_listener_.Get(); current;
       current = current->next()) {
    if (current != entry) {
      previous = current;
      continue;
    }
    if (previous) {
      previous->set_next(entry->next());
    } else {
      first_listener_ = entry->next();
    }
    if (last_listener_ == entry) {
      last_listener_ = previous;
    }
    entry->set_next(nullptr);
    ListenersChanged(entry->type());
    return;
  }
}

v8::Local<v8::Value> EventTarget::AttributeEventHandler(
    v8::Isolate* isolate,
    std::u16string_view type) const {
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (entry->attribute_handler() && entry->type() == type) {
      return entry->callback(isolate);
    }
  }
  return v8::Null(isolate);
}

void EventTarget::SetAttributeEventHandler(v8::Isolate* isolate,
                                           const std::u16string& type,
                                           v8::Local<v8::Value> handler) {
  EventListenerEntry* existing = nullptr;
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (entry->attribute_handler() && entry->type() == type) {
      existing = entry;
      break;
    }
  }
  if (!handler->IsObject()) {
    if (existing) {
      RemoveEntry(existing);
    }
    return;
  }
  if (existing) {
    existing->set_callback(isolate, handler.As<v8::Object>());
    return;
  }
  // Blink's EventTarget::SetAttributeEventListener() adds it as any listener,
  // which makes the target's EventTargetData.
  if (!AllowsUnloadListener(type)) {
    return;
  }
  has_event_target_data_ = true;
  EventListenerEntry* const entry =
      bindings::MakeGarbageCollected<EventListenerEntry>(
          isolate, type, handler.As<v8::Object>(), /*capture=*/false,
          /*passive=*/false, /*once=*/false, /*attribute_handler=*/true);
  if (last_listener_) {
    last_listener_->set_next(entry);
  } else {
    first_listener_ = entry;
  }
  last_listener_ = entry;
  ListenersChanged(type);
}

Node* EventTarget::ToNode() {
  return nullptr;
}

Window* EventTarget::ToWindow() {
  return nullptr;
}

bool EventTarget::IsTopLevelNode() {
  if (ToWindow()) {
    return true;
  }
  Node* const node = ToNode();
  if (!node) {
    return false;
  }
  Document& document = node->GetDocument();
  return node->IsDocumentNode() || document.DocumentElement() == node ||
         document.Body() == node;
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
  // Whether a listener canceled it, not whether a default event handler
  // handled it.
  return DispatchEventInternal(event) !=
         DispatchEventResult::kCanceledByEventHandler;
}

bool EventTarget::DispatchEvent(Event* event) {
  return DispatchEventWithResult(event) == DispatchEventResult::kNotCanceled;
}

DispatchEventResult EventTarget::DispatchEventWithResult(Event* event) {
  return DispatchEventInternal(event);
}

DispatchEventResult EventTarget::DispatchEventInternal(Event* event) {
  // Blink's EventTarget::DispatchEventInternal().
  event->SetTarget(this);
  event->SetCurrentTarget(this);
  event->SetPhase(Event::Phase::kAtTarget);
  // A target that never had a listener has no EventTargetData in Blink, and
  // its dispatch counts as not canceled, even for an event canceled before.
  const DispatchEventResult result =
      has_event_target_data_
          ? (FireEventListeners(event), GetDispatchEventResult(*event))
          : DispatchEventResult::kNotCanceled;
  if (bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
          ->features()
          .IsEnabled(
              bindings::RuntimeFeature::kClearCurrentTargetAfterDispatch)) {
    event->SetCurrentTarget(nullptr);
  }
  event->SetPhase(Event::Phase::kNone);
  return result;
}

bool EventTarget::DispatchEventWithTarget(Event* event, EventTarget* target) {
  // Blink's LocalDOMWindow::DispatchEvent(), which leaves the current target
  // and the phase as they are.
  event->SetTarget(target);
  event->SetCurrentTarget(this);
  event->SetPhase(Event::Phase::kAtTarget);
  if (!has_event_target_data_) {
    return true;
  }
  FireEventListeners(event);
  return GetDispatchEventResult(*event) == DispatchEventResult::kNotCanceled;
}

// static
DispatchEventResult EventTarget::GetDispatchEventResult(const Event& event) {
  if (event.DefaultPrevented()) {
    return DispatchEventResult::kCanceledByEventHandler;
  }
  if (event.DefaultHandled()) {
    return DispatchEventResult::kCanceledByDefaultEventHandler;
  }
  return DispatchEventResult::kNotCanceled;
}

bool EventTarget::FireEventListeners(Event* event) {
  if (!has_event_target_data_) {
    return false;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Don't reenter V8 once its execution is terminating.
  if (isolate->IsExecutionTerminating()) {
    return false;
  }
  // A copy, so that listeners that a listener adds wait for the next event,
  // and Persistent, so that the collector keeps them while scripts run.
  std::vector<cppgc::Persistent<EventListenerEntry>> entries;
  for (EventListenerEntry* entry = first_listener_.Get(); entry;
       entry = entry->next()) {
    if (entry->type() == event->Type()) {
      entries.emplace_back(entry);
    }
  }
  if (entries.empty()) {
    return false;
  }
  const v8::Local<v8::Value> current_target = ToV8(isolate);
  const v8::Local<v8::Value> event_wrapper = event->ToV8(isolate);
  // JSBasedEventListener::Invoke(): while a listener runs, its window's
  // current event is the event, which window.event gives, unless the listener
  // is in a shadow tree, and then what it was before.
  Window* const window =
      isolate->InContext()
          ? bindings::V8Window::ToWrappable(
                isolate, isolate->GetCurrentContext()->Global())
          : nullptr;
  bool fired = false;
  for (const cppgc::Persistent<EventListenerEntry>& entry : entries) {
    if (event->ImmediatePropagationStopped()) {
      break;
    }
    if (entry->removed() || !ShouldFire(*entry, *event)) {
      continue;
    }
    if (entry->once()) {
      RemoveEntry(entry.Get());
    }
    event->SetHandlingPassive(PassiveModeOf(*entry));
    const cppgc::Persistent<Event> previous_event(
        window ? window->CurrentEvent() : nullptr);
    if (window && !event->InvocationTargetInShadowTree()) {
      window->SetCurrentEvent(event);
    }
    if (entry->attribute_handler()) {
      CallAttributeHandler(isolate, entry->callback(isolate), *this,
                           current_target, event_wrapper, event);
    } else {
      CallListener(isolate, entry->callback(isolate), current_target,
                   event_wrapper);
    }
    fired = true;
    if (window) {
      window->SetCurrentEvent(previous_event.Get());
    }
    event->SetHandlingPassive(Event::PassiveMode::kNotPassive);
    if (isolate->IsExecutionTerminating()) {
      break;
    }
  }
  return fired;
}

}  // namespace mitto
