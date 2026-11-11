// Throws an error in the context of one binding call, so that its message says
// which call failed, as Chrome's do: "Failed to execute 'x' on 'Y': ...".
//
// An exception that V8 throws on its own, such as when a getter that a
// conversion calls throws, goes to the page as it is, with no context, as in
// Chrome.
//
// Upstream: third_party/blink/renderer/platform/bindings/exception_state.h

#ifndef MITTO_BINDINGS_EXCEPTION_STATE_H_
#define MITTO_BINDINGS_EXCEPTION_STATE_H_

#include <string_view>

#include "base/memory/raw_ptr.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-forward.h"

namespace mitto::bindings {

class ExceptionState {
 public:
  // `property_name` is null for a constructor.
  ExceptionState(v8::Isolate* isolate,
                 v8::ExceptionContext context,
                 const char* interface_name,
                 const char* property_name);
  ExceptionState(const ExceptionState&) = delete;
  ExceptionState& operator=(const ExceptionState&) = delete;
  ~ExceptionState();

  void ThrowTypeError(std::string_view message);
  void ThrowRangeError(std::string_view message);
  // Throws a DOMException with `name`, such as "InvalidStateError".
  void ThrowDOMException(std::string_view name, std::string_view message);

  bool HadException() const { return had_exception_; }

 private:
  std::string WithContext(std::string_view message) const;

  const raw_ptr<v8::Isolate> isolate_;
  const v8::ExceptionContext context_;
  const char* const interface_name_;
  const char* const property_name_;
  bool had_exception_ = false;
};

// Throws a TypeError with `message`, with no context.
void ThrowTypeError(v8::Isolate* isolate, std::string_view message);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_EXCEPTION_STATE_H_
