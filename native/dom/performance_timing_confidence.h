// Navigation Timing's PerformanceTimingConfidence: how sure the browser is
// that the navigation's times are typical, with the noise that keeps one
// navigation's answer private, which Chrome's browser process works out once
// the document is interactive.
//
// Upstream: third_party/blink/renderer/core/timing/performance_timing_confidence.cc
// Upstream: content/browser/renderer_host/randomized_confidence_utils.cc

#ifndef MITTO_DOM_PERFORMANCE_TIMING_CONFIDENCE_H_
#define MITTO_DOM_PERFORMANCE_TIMING_CONFIDENCE_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class PerformanceTimingConfidence final : public bindings::ScriptWrappable {
 public:
  // The rate at which Chrome answers at random, from its epsilon of 1.1:
  // 2 / (1 + e^1.1), rounded to 7 places.
  static constexpr double kRandomizedTriggerRate = 0.4994798;

  // GenerateRandomizedConfidenceLevel(): whether the answer for a navigation
  // whose confidence is high is "high", which it is unless a coin says to
  // answer at random and a second says "low".
  static bool DrawHighConfidence();

  PerformanceTimingConfidence(double randomized_trigger_rate, bool high);
  ~PerformanceTimingConfidence() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double RandomizedTriggerRate() const { return randomized_trigger_rate_; }
  std::u16string Value() const { return high_ ? u"high" : u"low"; }
  v8::Local<v8::Object> ToJSON() const;

 private:
  const double randomized_trigger_rate_;
  const bool high_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_TIMING_CONFIDENCE_H_
