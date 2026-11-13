#include "mitto/dom/performance_timing_confidence.h"

#include "base/rand_util.h"
#include "mitto/bindings/v8_performance_timing_confidence.h"
#include "mitto/dom/performance_entry.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
bool PerformanceTimingConfidence::DrawHighConfidence() {
  // The first coin is a float, as Chrome draws it.
  const float first_coin_flip = static_cast<float>(base::RandDouble());
  if (first_coin_flip < kRandomizedTriggerRate) {
    return base::RandBool();
  }
  return true;
}

PerformanceTimingConfidence::PerformanceTimingConfidence(
    double randomized_trigger_rate,
    bool high)
    : randomized_trigger_rate_(randomized_trigger_rate), high_(high) {}

PerformanceTimingConfidence::~PerformanceTimingConfidence() = default;

const bindings::WrapperTypeInfo*
PerformanceTimingConfidence::GetWrapperTypeInfo() const {
  return &bindings::V8PerformanceTimingConfidence::kWrapperTypeInfo;
}

v8::Local<v8::Object> PerformanceTimingConfidence::ToJSON() const {
  V8ObjectBuilder builder(v8::Isolate::GetCurrent());
  builder.AddNumber("randomizedTriggerRate", RandomizedTriggerRate());
  builder.AddString("value", Value());
  return builder.Result();
}

}  // namespace mitto
