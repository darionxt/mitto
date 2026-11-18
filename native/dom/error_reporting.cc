#include "mitto/dom/error_reporting.h"

#include <stdint.h>

#include <string>
#include <vector>

#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/promise_rejection_event.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The execution context of the current context, a window or a worker's global
// scope, while the Page still runs its scripts, as Blink's
// ScriptState::ContextIsValid() asks.
ExecutionContext* CurrentContext(v8::Isolate* isolate) {
  if (!isolate->InContext()) {
    return nullptr;
  }
  ExecutionContext* const context =
      ExecutionContext::From(isolate, isolate->GetCurrentContext());
  if (!context || !context->HasContext()) {
    return nullptr;
  }
  if (context->IsWindow() && !static_cast<Window*>(context)->Document()) {
    return nullptr;
  }
  return context;
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
// makes it.
ErrorEvent* ErrorEventOf(v8::Isolate* isolate,
                         ExecutionContext& execution_context,
                         v8::Local<v8::Message> message,
                         v8::Local<v8::Value> exception) {
  ErrorMessageInfo info =
      DescribeErrorMessage(isolate, execution_context, message);
  return ErrorEvent::CreateForScriptError(info.message, info.filename,
                                          info.line, info.column, exception);
}

}  // namespace

ErrorMessageInfo::ErrorMessageInfo() = default;
ErrorMessageInfo::ErrorMessageInfo(const ErrorMessageInfo&) = default;
ErrorMessageInfo& ErrorMessageInfo::operator=(const ErrorMessageInfo&) =
    default;
ErrorMessageInfo::~ErrorMessageInfo() = default;

ErrorMessageInfo DescribeErrorMessage(v8::Isolate* isolate,
                                      ExecutionContext& execution_context,
                                      v8::Local<v8::Message> message) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  ErrorMessageInfo info;
  info.message = ToU16String(isolate, message->Get());
  info.filename =
      ToU16String(isolate, message->GetScriptOrigin().ResourceName());
  if (info.filename.empty()) {
    info.filename = execution_context.Url();
  }
  int line = 0;
  int column = 0;
  if (message->GetLineNumber(context).To(&line) &&
      message->GetStartColumn(context).To(&column)) {
    ++column;
  }
  info.line = static_cast<uint32_t>(line < 0 ? 0 : line);
  info.column = static_cast<uint32_t>(column < 0 ? 0 : column);
  return info;
}

void ReportUncaughtError(v8::Isolate* isolate,
                         v8::Local<v8::Message> message,
                         v8::Local<v8::Value> exception,
                         ReportToConsole to_console) {
  ExecutionContext* const context = CurrentContext(isolate);
  if (!context) {
    to_console(exception);
    return;
  }
  ExecutionContext::ErrorEventState& state = context->GetErrorEventState();
  if (state.draining_pending_exceptions) {
    return;
  }
  if (state.in_dispatch_error_event) {
    state.pending_exceptions.emplace_back(isolate, exception);
    return;
  }
  bool canceled = false;
  if (!message.IsEmpty()) {
    const v8::HandleScope handle_scope(isolate);
    // A script whose errors are muted shows the context's listeners nothing
    // of its error.
    const cppgc::Persistent<ErrorEvent> event(
        message->IsSharedCrossOrigin()
            ? ErrorEventOf(isolate, *context, message, exception)
            : ErrorEvent::CreateSanitized());
    event->SetTrusted(true);
    state.in_dispatch_error_event = true;
    context->ErrorEventTarget()->DispatchEvent(event.Get());
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
  ExecutionContext* const context = CurrentContext(isolate);
  if (!context) {
    return true;
  }
  const cppgc::Persistent<PromiseRejectionEvent> event(
      PromiseRejectionEvent::CreateForRejection(u"unhandledrejection",
                                                /*cancelable=*/true, promise,
                                                reason));
  event->SetTrusted(true);
  return context->ErrorEventTarget()->DispatchEvent(event.Get());
}

v8::ScriptOrigin ClassicScriptOrigin(v8::Local<v8::Value> name,
                                     bool muted_errors,
                                     int line_offset,
                                     int column_offset) {
  return v8::ScriptOrigin(name, line_offset, column_offset,
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
  ExecutionContext* const context = CurrentContext(isolate);
  if (!context) {
    return;
  }
  const cppgc::Persistent<PromiseRejectionEvent> event(
      PromiseRejectionEvent::CreateForRejection(u"rejectionhandled",
                                                /*cancelable=*/false, promise,
                                                reason));
  event->SetTrusted(true);
  context->ErrorEventTarget()->DispatchEvent(event.Get());
}

}  // namespace mitto
