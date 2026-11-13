#include "mitto/dom/performance_measure.h"

#include <utility>

#include "mitto/bindings/v8_performance_measure.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

PerformanceMeasure::PerformanceMeasure(
    std::u16string name,
    double start_time,
    double end_time,
    std::optional<SerializedScriptValue> detail,
    uint64_t navigation_id)
    : PerformanceEntry(std::move(name), start_time, end_time, navigation_id),
      detail_(std::move(detail)) {}

PerformanceMeasure::~PerformanceMeasure() = default;

const bindings::WrapperTypeInfo* PerformanceMeasure::GetWrapperTypeInfo()
    const {
  return &bindings::V8PerformanceMeasure::kWrapperTypeInfo;
}

void PerformanceMeasure::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(deserialized_detail_);
  PerformanceEntry::Trace(visitor);
}

PerformanceEntry::Kind PerformanceMeasure::GetKind() const {
  return kMeasure;
}

std::u16string_view PerformanceMeasure::TypeName() const {
  return u"measure";
}

v8::Local<v8::Value> PerformanceMeasure::Detail() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!detail_) {
    return v8::Null(isolate);
  }
  if (deserialized_detail_.IsEmpty()) {
    deserialized_detail_.Reset(isolate, detail_->Deserialize(isolate));
  }
  return deserialized_detail_.Get(isolate);
}

}  // namespace mitto
