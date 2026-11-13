#include "mitto/dom/quota_exceeded_error.h"

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/v8_quota_exceeded_error.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"

namespace mitto {

// static
QuotaExceededError* QuotaExceededError::Create(
    const std::u16string& message,
    const bindings::QuotaExceededErrorOptions& options) {
  return Create(message, options.quota, options.requested);
}

// static
QuotaExceededError* QuotaExceededError::Create(
    const std::u16string& message,
    std::optional<double> quota,
    std::optional<double> requested) {
  return bindings::MakeGarbageCollected<QuotaExceededError>(message, quota,
                                                            requested);
}

// static
void QuotaExceededError::Throw(bindings::ExceptionState& exception_state,
                               std::string_view message,
                               std::optional<double> quota,
                               std::optional<double> requested) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!bindings::PerIsolateData::From(isolate)->features().IsEnabled(
          bindings::RuntimeFeature::kQuotaExceededErrorUpdate)) {
    exception_state.ThrowDOMException("QuotaExceededError", message);
    return;
  }
  // Blink's V8ThrowDOMException::AttachStackProperty(), as for a
  // DOMException that the bindings throw.
  const v8::Local<v8::Object> wrapper =
      Create(base::UTF8ToUTF16(message), quota, requested)->ToV8(isolate);
  if (!isolate->IsExecutionTerminating()) {
    v8::Exception::CaptureStackTrace(isolate->GetCurrentContext(), wrapper);
  }
  exception_state.ThrowDOMException(wrapper);
}

QuotaExceededError::QuotaExceededError(const std::u16string& message,
                                       std::optional<double> quota,
                                       std::optional<double> requested)
    : DOMException(message, u"QuotaExceededError"),
      quota_(quota),
      requested_(requested) {}

QuotaExceededError::~QuotaExceededError() = default;

const bindings::WrapperTypeInfo* QuotaExceededError::GetWrapperTypeInfo()
    const {
  return &bindings::V8QuotaExceededError::kWrapperTypeInfo;
}

}  // namespace mitto
