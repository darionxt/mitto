// Throws an error from a binding call, and remembers that it did.
//
// The message is the error itself, such as "1 argument required, but only 0
// present.". As in Chrome, the call's context, such as "Failed to execute 'x'
// on 'Y': ", goes in front of it when it leaves the call (exception_context.h),
// as it does for an error that V8 throws during the call.
//
// Upstream: third_party/blink/renderer/platform/bindings/exception_state.h
// Upstream: third_party/blink/renderer/platform/bindings/exception_state.cc

#ifndef MITTO_BINDINGS_EXCEPTION_STATE_H_
#define MITTO_BINDINGS_EXCEPTION_STATE_H_

#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-forward.h"

namespace mitto::bindings {

class ExceptionState {
 public:
  explicit ExceptionState(v8::Isolate* isolate);
  ExceptionState(const ExceptionState&) = delete;
  ExceptionState& operator=(const ExceptionState&) = delete;
  ~ExceptionState();

  void ThrowTypeError(std::string_view message);
  void ThrowRangeError(std::string_view message);
  // Throws a DOMException with `name`, such as "InvalidStateError".
  void ThrowDOMException(std::string_view name, std::string_view message);
  // Throws `exception`, a DOMException that the DOM made, such as a
  // QuotaExceededError, as Blink's ThrowDOMException() overload that takes
  // one does.
  void ThrowDOMException(v8::Local<v8::Value> exception);
  // Throws again what `try_catch` caught during the call, such as an error
  // that a page's own function threw, as Blink's RethrowV8Exception() does,
  // even if this state threw it.
  void RethrowV8Exception(v8::TryCatch& try_catch);

  bool HadException() const { return had_exception_; }
  v8::Isolate* GetIsolate() const { return isolate_; }

 private:
  const raw_ptr<v8::Isolate> isolate_;
  bool had_exception_ = false;
};

// Catches what the calls it encloses throw, and throws it again through
// `exception_state` when it goes, so that the caller sees that the call threw,
// as Blink's TryRethrowScope does.
class TryRethrowScope {
 public:
  TryRethrowScope(v8::Isolate* isolate, ExceptionState& exception_state);
  TryRethrowScope(const TryRethrowScope&) = delete;
  TryRethrowScope& operator=(const TryRethrowScope&) = delete;
  ~TryRethrowScope();

  bool HasCaught() const { return try_catch_.HasCaught(); }
  v8::Local<v8::Value> GetException() const { return try_catch_.Exception(); }

 private:
  v8::TryCatch try_catch_;
  const raw_ref<ExceptionState> exception_state_;
};

// Throws a TypeError with `message`.
void ThrowTypeError(v8::Isolate* isolate, std::string_view message);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_EXCEPTION_STATE_H_
