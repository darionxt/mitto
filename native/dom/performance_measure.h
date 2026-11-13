// User Timing's PerformanceMeasure, which performance.measure() makes, with
// its detail as PerformanceMark keeps one.
//
// Upstream: third_party/blink/renderer/core/timing/performance_measure.h
// Upstream: third_party/blink/renderer/core/timing/performance_measure.cc

#ifndef MITTO_DOM_PERFORMANCE_MEASURE_H_
#define MITTO_DOM_PERFORMANCE_MEASURE_H_

#include <optional>
#include <string>

#include "mitto/dom/performance_entry.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class PerformanceMeasure final : public PerformanceEntry {
 public:
  PerformanceMeasure(std::u16string name,
                     double start_time,
                     double end_time,
                     std::optional<SerializedScriptValue> detail,
                     uint64_t navigation_id);
  ~PerformanceMeasure() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Detail();

  // PerformanceEntry:
  Kind GetKind() const override;
  std::u16string_view TypeName() const override;

 private:
  const std::optional<SerializedScriptValue> detail_;
  v8::TracedReference<v8::Value> deserialized_detail_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_MEASURE_H_
