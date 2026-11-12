// Coarsens the times that performance.now() and the like give a page, as
// Chrome coarsens them against timing attacks: to 100 microseconds, or 5 for a
// cross-origin isolated context, which a Page never is. Each interval has a
// threshold that a secret decides, past which a time becomes the next
// interval's start, so that two times cannot be told apart within one.
//
// Upstream: third_party/blink/renderer/core/timing/time_clamper.h
// Upstream: third_party/blink/renderer/core/timing/time_clamper.cc

#ifndef MITTO_DOM_TIME_CLAMPER_H_
#define MITTO_DOM_TIME_CLAMPER_H_

#include <stdint.h>

#include "base/time/time.h"

namespace mitto {

class TimeClamper {
 public:
  static constexpr int kCoarseResolutionMicroseconds = 100;
  static constexpr int kFineResolutionMicroseconds = 5;

  // With a secret of its own, as each Chrome renderer has one.
  TimeClamper();
  // With `secret`, for tests.
  explicit TimeClamper(uint64_t secret);

  TimeClamper(const TimeClamper&) = delete;
  TimeClamper& operator=(const TimeClamper&) = delete;

  ~TimeClamper();

  base::TimeDelta ClampTimeResolution(
      base::TimeDelta time,
      bool cross_origin_isolated_capability) const;

 private:
  double ThresholdFor(int64_t clamped_time, int resolution) const;

  const uint64_t secret_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TIME_CLAMPER_H_
