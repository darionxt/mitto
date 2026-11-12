#include "mitto/page/wall_clock_clamper.h"

#include <stdlib.h>

#include <algorithm>
#include <limits>

#include "base/check_op.h"
#include "base/rand_util.h"

namespace mitto {
namespace {

uint64_t MurmurHash3(uint64_t value) {
  value ^= value >> 33;
  value *= uint64_t{0xFF51AFD7ED558CCD};
  value ^= value >> 33;
  value *= uint64_t{0xC4CEB9FE1A85EC53};
  value ^= value >> 33;
  return value;
}

}  // namespace

WallClockClamper::WallClockClamper() : secret_(base::RandUint64()) {}

WallClockClamper::WallClockClamper(uint64_t secret) : secret_(secret) {}

WallClockClamper::~WallClockClamper() = default;

int64_t WallClockClamper::ClampToMillis(base::Time time) const {
  // Jitter only matters within kResolutionMicros of the next millisecond:
  // anywhere else, the clamped time rounds down to the same millisecond.
  const int64_t now_micros = (time - base::Time::UnixEpoch()).InMicroseconds();
  const int64_t micros = now_micros % 1000;
  // A machine set before the epoch gives a negative remainder.
  if (llabs(micros) + kResolutionMicros < 1000) {
    return now_micros / 1000;
  }
  return ClampTimeResolution(now_micros) / 1000;
}

double WallClockClamper::ClampToMillisHighResolution(base::Time time) const {
  const int64_t clamped_time =
      ClampTimeResolution((time - base::Time::UnixEpoch()).InMicroseconds());
  return static_cast<double>(clamped_time) / 1000.0;
}

int64_t WallClockClamper::ClampTimeResolution(int64_t time_micros) const {
  if (time_micros < 0) {
    return -ClampTimeResolutionPositiveValue(-time_micros);
  }
  return ClampTimeResolutionPositiveValue(time_micros);
}

int64_t WallClockClamper::ClampTimeResolutionPositiveValue(
    int64_t time_micros) const {
  DCHECK_GE(time_micros, 0);
  // Each interval has a threshold of its own: the time is the interval's start
  // below it, and the next interval's start from it on.
  const int64_t interval = time_micros / kResolutionMicros;
  const int64_t clamped_time_micros = interval * kResolutionMicros;
  if (time_micros - clamped_time_micros < ThresholdFor(clamped_time_micros)) {
    return clamped_time_micros;
  }
  return clamped_time_micros + kResolutionMicros;
}

int64_t WallClockClamper::ThresholdFor(int64_t clamped_time) const {
  // A number from 0 to kResolutionMicros, without floating point:
  // random / (UINT64_MAX / kResolutionMicros).
  const uint64_t random_value =
      MurmurHash3(static_cast<uint64_t>(clamped_time) ^ secret_);
  return std::min(
      static_cast<int64_t>(random_value /
                           (std::numeric_limits<uint64_t>::max() /
                            static_cast<uint64_t>(kResolutionMicros))),
      kResolutionMicros);
}

}  // namespace mitto
