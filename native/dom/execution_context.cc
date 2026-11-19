#include "mitto/dom/execution_context.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The string's code units as bytes, or nullopt if one is beyond Latin-1.
std::optional<std::string> ToLatin1(const std::u16string& text) {
  std::string bytes;
  bytes.reserve(text.size());
  for (const char16_t c : text) {
    if (c > 0xFF) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<char>(c));
  }
  return bytes;
}

}  // namespace

// static
ExecutionContext& ExecutionContext::Current() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  ExecutionContext* const context = From(isolate, isolate->GetCurrentContext());
  CHECK(context);
  return *context;
}

// static
ExecutionContext* ExecutionContext::From(v8::Isolate* isolate,
                                         v8::Local<v8::Context> context) {
  return bindings::V8Window::ToWrappable(isolate, context->Global());
}

ExecutionContext::ExecutionContext() = default;
ExecutionContext::~ExecutionContext() = default;

bool ExecutionContext::IsWindow() const {
  return false;
}

void ExecutionContext::SetStart(const PageStart& start,
                                const PageConnection& connection) {
  start_ = start;
  connection_ = connection;
}

void ExecutionContext::ReportError(v8::Local<v8::Value> e) {
  // WindowOrWorkerGlobalScope::reportError():
  // V8ScriptRunner::ReportException(), with V8's message of `e`, which says
  // where an error was made, and else where reportError() was called.
  bindings::ReportException(v8::Isolate::GetCurrent(), e);
}

void ExecutionContext::AddConsoleMessage(ConsoleLevel level, std::string text) {
  if (console_sink_) {
    console_sink_.Run(level, std::move(text));
  }
}

int32_t ExecutionContext::SetTimeout(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetTimeout(handler, timeout, arguments) : 0;
}

int32_t ExecutionContext::SetTimeout(
    const std::u16string& handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments,
    bindings::ExceptionState& exception_state) {
  // As Chrome, a string handler gets none of the arguments.
  return timers_ ? timers_->SetTimeout(handler, timeout) : 0;
}

void ExecutionContext::ClearTimeout(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

int32_t ExecutionContext::SetInterval(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetInterval(handler, timeout, arguments) : 0;
}

int32_t ExecutionContext::SetInterval(
    const std::u16string& handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments,
    bindings::ExceptionState& exception_state) {
  return timers_ ? timers_->SetInterval(handler, timeout) : 0;
}

void ExecutionContext::ClearInterval(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

void ExecutionContext::SetTimers(DOMTimerCoordinator* timers) {
  timers_ = timers;
  if (timers) {
    return;
  }
  // The context goes, and with it the signals' timers and what else goes
  // with it.
  std::map<uint64_t, base::OnceClosure> signal_timers;
  signal_timers.swap(signal_timers_);
  for (auto& [id, cancel] : signal_timers) {
    std::move(cancel).Run();
  }
  std::vector<base::OnceClosure> callbacks;
  callbacks.swap(context_destroyed_callbacks_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
}

void ExecutionContext::AddContextDestroyedCallback(base::OnceClosure callback) {
  context_destroyed_callbacks_.push_back(std::move(callback));
}

uint64_t ExecutionContext::AddSignalTimer(base::OnceClosure cancel) {
  const uint64_t id = ++next_signal_timer_;
  signal_timers_.emplace(id, std::move(cancel));
  return id;
}

void ExecutionContext::RemoveSignalTimer(uint64_t id) {
  signal_timers_.erase(id);
}

size_t ExecutionContext::TimerCount() const {
  return (timers_ ? timers_->size() : 0) + signal_timers_.size();
}

void ExecutionContext::QueueMicrotask(v8::Local<v8::Function> callback) {
  // Blink queues a microtask of its own, which calls the callback and reports
  // what it throws with where it was thrown, where V8's own microtask of a
  // function would report where an error was made.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  isolate->EnqueueMicrotask(&RunQueuedMicrotask,
                            new v8::Global<v8::Function>(isolate, callback));
}

// static
void ExecutionContext::RunQueuedMicrotask(void* data) {
  // V8VoidFunction::InvokeAndReportException(), with undefined for `this`.
  const std::unique_ptr<v8::Global<v8::Function>> held(
      static_cast<v8::Global<v8::Function>*>(data));
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Function> callback = held->Get(isolate);
  v8::Local<v8::Context> context;
  if (!callback->GetCreationContext(isolate).ToLocal(&context)) {
    return;
  }
  const v8::Context::Scope context_scope(context);
  // Not once the context has gone.
  ExecutionContext* const execution_context = From(isolate, context);
  if (!execution_context || !execution_context->HasContext()) {
    return;
  }
  const v8::TryCatch try_catch(isolate);
  if (callback->Call(context, v8::Undefined(isolate), 0, nullptr).IsEmpty() &&
      try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
}

// Upstream: third_party/blink/renderer/platform/wtf/text/base64.cc
std::u16string ExecutionContext::Btoa(
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  const std::optional<std::string> bytes = ToLatin1(data);
  if (!bytes) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be encoded contains characters outside of the Latin1 "
        "range.");
    return {};
  }
  return base::ASCIIToUTF16(base::Base64Encode(*bytes));
}

// The Infra Standard's forgiving-base64 decode, as Blink's Base64Decode()
// runs it: modp_b64's forgiving policy, and again without ASCII whitespace if
// that fails, which base::Base64Decode() does the same way.
std::u16string ExecutionContext::Atob(
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  const std::optional<std::string> bytes = ToLatin1(data);
  if (!bytes) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be decoded contains characters outside of the Latin1 "
        "range.");
    return {};
  }
  std::string decoded;
  if (!base::Base64Decode(*bytes, &decoded,
                          base::Base64DecodePolicy::kForgiving)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be decoded is not correctly encoded.");
    return {};
  }
  // Each byte is a code unit, as Blink's String of the bytes is Latin-1.
  std::u16string text;
  text.reserve(decoded.size());
  for (const char byte : decoded) {
    text.push_back(static_cast<uint8_t>(byte));
  }
  return text;
}

}  // namespace mitto
