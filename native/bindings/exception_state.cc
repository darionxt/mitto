#include "mitto/bindings/exception_state.h"

#include <string>

#include "base/check.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

ExceptionState::ExceptionState(v8::Isolate* isolate,
                               v8::ExceptionContext context,
                               const char* interface_name,
                               const char* property_name)
    : isolate_(isolate),
      context_(context),
      interface_name_(interface_name),
      property_name_(property_name) {}

ExceptionState::~ExceptionState() = default;

std::string ExceptionState::WithContext(std::string_view message) const {
  return ExceptionMessages::AddContextToMessage(
      context_, interface_name_, property_name_ ? property_name_ : "",
      message);
}

void ExceptionState::ThrowTypeError(std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  isolate_->ThrowException(
      v8::Exception::TypeError(ToV8StringFromUtf8(isolate_, WithContext(message))));
}

void ExceptionState::ThrowRangeError(std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  isolate_->ThrowException(v8::Exception::RangeError(
      ToV8StringFromUtf8(isolate_, WithContext(message))));
}

void ExceptionState::ThrowDOMException(std::string_view name,
                                       std::string_view message) {
  CHECK(!had_exception_);
  had_exception_ = true;
  const PerIsolateData::DOMExceptionFactory factory =
      PerIsolateData::From(isolate_)->dom_exception_factory();
  CHECK(factory);
  isolate_->ThrowException(factory(isolate_, name, WithContext(message)));
}

void ThrowTypeError(v8::Isolate* isolate, std::string_view message) {
  isolate->ThrowException(
      v8::Exception::TypeError(ToV8StringFromUtf8(isolate, message)));
}

}  // namespace mitto::bindings
