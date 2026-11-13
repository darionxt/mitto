#include "mitto/dom/performance_mark.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_performance_mark.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/performance_timing.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
PerformanceMark* PerformanceMark::Create(
    const std::u16string& mark_name,
    const bindings::PerformanceMarkOptions& options,
    bindings::ExceptionState& exception_state) {
  Performance& performance = *Window::Current().Performance();
  double start = 0;
  if (options.start_time) {
    start = *options.start_time;
    if (start < 0) {
      exception_state.ThrowTypeError(
          base::StrCat({"'", base::UTF16ToUTF8(mark_name),
                        "' cannot have a negative start time."}));
      return nullptr;
    }
  } else {
    start = performance.Now();
  }
  // A window's mark may not take the name of a PerformanceTiming attribute.
  if (PerformanceTiming::IsAttributeName(mark_name)) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"'", base::UTF16ToUTF8(mark_name),
                      "' is part of the PerformanceTiming interface, and "
                      "cannot be used as a mark name."}));
    return nullptr;
  }
  std::optional<SerializedScriptValue> detail;
  if (options.detail) {
    detail = SerializedScriptValue::Serialize(v8::Isolate::GetCurrent(),
                                              *options.detail);
    if (!detail) {
      return nullptr;
    }
  }
  return bindings::MakeGarbageCollected<PerformanceMark>(
      mark_name, start, std::move(detail), performance.NavigationId());
}

PerformanceMark::PerformanceMark(std::u16string name,
                                 double start_time,
                                 std::optional<SerializedScriptValue> detail,
                                 uint64_t navigation_id)
    : PerformanceEntry(std::move(name), start_time, start_time, navigation_id),
      detail_(std::move(detail)) {}

PerformanceMark::~PerformanceMark() = default;

const bindings::WrapperTypeInfo* PerformanceMark::GetWrapperTypeInfo() const {
  return &bindings::V8PerformanceMark::kWrapperTypeInfo;
}

void PerformanceMark::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(deserialized_detail_);
  PerformanceEntry::Trace(visitor);
}

PerformanceEntry::Kind PerformanceMark::GetKind() const {
  return kMark;
}

std::u16string_view PerformanceMark::TypeName() const {
  return u"mark";
}

v8::Local<v8::Value> PerformanceMark::Detail() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!detail_) {
    return v8::Null(isolate);
  }
  // A Page has one context at a time, so one copy stands for Blink's copy for
  // each context.
  if (deserialized_detail_.IsEmpty()) {
    deserialized_detail_.Reset(isolate, detail_->Deserialize(isolate));
  }
  return deserialized_detail_.Get(isolate);
}

}  // namespace mitto
