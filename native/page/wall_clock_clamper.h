// Coarsens the wall clock that V8's Date reads, as Chrome's renderers do, so
// that Date.now() and new Date() cannot time what happens within a few
// microseconds of a millisecond's boundary.
//
// Upstream: gin/time_clamper.h
// Upstream: gin/time_clamper.cc

#ifndef MITTO_PAGE_WALL_CLOCK_CLAMPER_H_
#define MITTO_PAGE_WALL_CLOCK_CLAMPER_H_

#include <stdint.h>

#include "base/time/time.h"

namespace mitto {

class WallClockClamper {
 public:
  // The resolution of Chrome's on a desktop. Android's is 100.
  static constexpr int64_t kResolutionMicros = 5;

  // With a secret of its own, which decides where each interval's threshold
  // lies, as each Chrome renderer has one.
  WallClockClamper();
  // With `secret`, for tests.
  explicit WallClockClamper(uint64_t secret);

  WallClockClamper(const WallClockClamper&) = delete;
  WallClockClamper& operator=(const WallClockClamper&) = delete;

  ~WallClockClamper();

  // `time` in milliseconds since the Unix epoch, with millisecond precision:
  // the time itself, but within kResolutionMicros of the next millisecond,
  // where it becomes that millisecond past a threshold that the secret
  // decides.
  int64_t ClampToMillis(base::Time time) const;

  // `time` in milliseconds since the Unix epoch, coarsened to
  // kResolutionMicros.
  double ClampToMillisHighResolution(base::Time time) const;

 private:
  int64_t ClampTimeResolution(int64_t time_micros) const;
  int64_t ClampTimeResolutionPositiveValue(int64_t time_micros) const;
  int64_t ThresholdFor(int64_t clamped_time) const;

  const uint64_t secret_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_WALL_CLOCK_CLAMPER_H_
