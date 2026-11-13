#include "mitto/bindings/exception_state.h"

#include "base/check.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

namespace mitto::bindings {

ExceptionState::ExceptionState(v8::Isolate* isolate) : isolate_(isolate) {}

ExceptionState::~ExceptionState() = default;

void ExceptionState::ThrowTypeError(std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  isolate_->ThrowException(
      v8::Exception::TypeError(ToV8StringFromUtf8(isolate_, message)));
}

void ExceptionState::ThrowRangeError(std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  isolate_->ThrowException(
      v8::Exception::RangeError(ToV8StringFromUtf8(isolate_, message)));
}

void ExceptionState::ThrowDOMException(std::string_view name,
                                       std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  const PerIsolateData::DOMExceptionHooks& hooks =
      PerIsolateData::From(isolate_)->dom_exception_hooks();
  CHECK(hooks.create);
  isolate_->ThrowException(hooks.create(isolate_, name, message));
}

void ExceptionState::ThrowDOMException(v8::Local<v8::Value> exception) {
  CHECK(!had_exception_);
  had_exception_ = true;
  isolate_->ThrowException(exception);
}

void ThrowTypeError(v8::Isolate* isolate, std::string_view message) {
  isolate->ThrowException(
      v8::Exception::TypeError(ToV8StringFromUtf8(isolate, message)));
}

}  // namespace mitto::bindings
