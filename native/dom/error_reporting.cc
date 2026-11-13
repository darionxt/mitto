#include "mitto/dom/error_reporting.h"

#include <stdint.h>

#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/promise_rejection_event.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// ExecutionContext's state of its error events. A Page runs one window's
// scripts at a time, on one thread.
struct ErrorEventState {
  // Whether the window's error listeners run, which queues an error that
  // they throw for the console alone.
  bool in_dispatch_error_event = false;
  // Whether those queued errors go to the console, which drops any error
  // thrown meanwhile.
  bool draining_pending_exceptions = false;
  std::vector<v8::Global<v8::Value>> pending_exceptions;
};

ErrorEventState& State() {
  static base::NoDestructor<ErrorEventState> state;
  return *state;
}

// The window of the current context, while the Page still runs its scripts,
// as Blink's ScriptState::ContextIsValid() asks.
Window* CurrentWindow(v8::Isolate* isolate) {
  if (!isolate->InContext()) {
    return nullptr;
  }
  Window* const window = bindings::V8Window::ToWrappable(
      isolate, isolate->GetCurrentContext()->Global());
  if (!window || !window->HasContext() || !window->Document()) {
    return nullptr;
  }
  return window;
}

std::u16string ToU16String(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsString()) {
    return std::u16string();
  }
  const v8::Local<v8::String> string = value.As<v8::String>();
  std::u16string text(static_cast<size_t>(string->Length()), u'\0');
  string->WriteV2(isolate, 0, string->Length(),
                  reinterpret_cast<uint16_t*>(text.data()));
  return text;
}

// The ErrorEvent of V8's message, as Blink's MessageHandlerInMainThread()
// makes it: the message's text, such as "Uncaught TypeError: x is not a
// function", and CaptureSourceLocation()'s place, the script's URL or else the
// document's, its line, and its column counted from 1, as no DevTools asks V8
// for stack traces of uncaught errors.
ErrorEvent* ErrorEventOf(v8::Isolate* isolate,
                         Window& window,
                         v8::Local<v8::Message> message,
                         v8::Local<v8::Value> exception) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::u16string filename =
      ToU16String(isolate, message->GetScriptOrigin().ResourceName());
  if (filename.empty()) {
    filename = window.GetDocument().UrlForBinding();
  }
  int line = 0;
  int column = 0;
  if (message->GetLineNumber(context).To(&line) &&
      message->GetStartColumn(context).To(&column)) {
    ++column;
  }
  return ErrorEvent::CreateForScriptError(
      ToU16String(isolate, message->Get()), filename,
      static_cast<uint32_t>(line < 0 ? 0 : line),
      static_cast<uint32_t>(column < 0 ? 0 : column), exception);
}

}  // namespace

void ReportUncaughtError(v8::Isolate* isolate,
                         v8::Local<v8::Message> message,
                         v8::Local<v8::Value> exception,
                         ReportToConsole to_console) {
  ErrorEventState& state = State();
  if (state.draining_pending_exceptions) {
    return;
  }
  if (state.in_dispatch_error_event) {
    state.pending_exceptions.emplace_back(isolate, exception);
    return;
  }
  bool canceled = false;
  Window* const window = CurrentWindow(isolate);
  if (window && !message.IsEmpty()) {
    const v8::HandleScope handle_scope(isolate);
    // A script whose errors are muted shows the window's listeners nothing
    // of its error.
    const cppgc::Persistent<ErrorEvent> event(
        message->IsSharedCrossOrigin()
            ? ErrorEventOf(isolate, *window, message, exception)
            : ErrorEvent::CreateSanitized());
    event->SetTrusted(true);
    state.in_dispatch_error_event = true;
    window->DispatchEvent(event.Get());
    state.in_dispatch_error_event = false;
    canceled = event->DefaultPrevented();
  }
  state.draining_pending_exceptions = true;
  if (!canceled) {
    to_console(exception);
  }
  std::vector<v8::Global<v8::Value>> pending;
  pending.swap(state.pending_exceptions);
  for (const v8::Global<v8::Value>& pending_exception : pending) {
    const v8::HandleScope handle_scope(isolate);
    to_console(pending_exception.Get(isolate));
  }
  state.draining_pending_exceptions = false;
}

bool RejectionFiresEvents(v8::Isolate* isolate, v8::Local<v8::Value> reason) {
  // Blink's PromiseRejectHandler(): V8's message of the reason, which for an
  // error says where it was made, and for another value where the script was
  // that rejected it.
  const v8::Local<v8::Message> message =
      v8::Exception::CreateMessage(isolate, reason);
  return !message.IsEmpty() && message->IsSharedCrossOrigin();
}

bool DispatchUnhandledRejection(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise,
                                v8::Local<v8::Value> reason) {
  Window* const window = CurrentWindow(isolate);
  if (!window) {
    return true;
  }
  const cppgc::Persistent<PromiseRejectionEvent> event(
      PromiseRejectionEvent::CreateForRejection(u"unhandledrejection",
                                                /*cancelable=*/true, promise,
                                                reason));
  event->SetTrusted(true);
  return window->DispatchEvent(event.Get());
}

v8::ScriptOrigin ClassicScriptOrigin(v8::Local<v8::Value> name,
                                     bool muted_errors) {
  return v8::ScriptOrigin(name, /*resource_line_offset=*/0,
                          /*resource_column_offset=*/0,
                          /*resource_is_shared_cross_origin=*/!muted_errors);
}

void ReportScriptException(v8::Isolate* isolate,
                           const v8::TryCatch& try_catch) {
  if (try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
}

void DispatchRejectionHandled(v8::Isolate* isolate,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> reason) {
  Window* const window = CurrentWindow(isolate);
  if (!window) {
    return;
  }
  const cppgc::Persistent<PromiseRejectionEvent> event(
      PromiseRejectionEvent::CreateForRejection(u"rejectionhandled",
                                                /*cancelable=*/false, promise,
                                                reason));
  event->SetTrusted(true);
  window->DispatchEvent(event.Get());
}

}  // namespace mitto
