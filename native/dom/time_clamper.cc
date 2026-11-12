#include "mitto/dom/time_clamper.h"

#include <algorithm>
#include <limits>

#include "base/bit_cast.h"
#include "base/rand_util.h"

namespace mitto {
namespace {

constexpr int64_t kTenLowerDigitsMod = 10000000000;

uint64_t MurmurHash3(uint64_t value) {
  value ^= value >> 33;
  value *= uint64_t{0xFF51AFD7ED558CCD};
  value ^= value >> 33;
  value *= uint64_t{0xC4CEB9FE1A85EC53};
  value ^= value >> 33;
  return value;
}

// A double in [0, 1) from the bits of `value`.
double ToDouble(uint64_t value) {
  constexpr uint64_t kExponentBits = uint64_t{0x3FF0000000000000};
  constexpr uint64_t kMantissaMask = uint64_t{0x000FFFFFFFFFFFFF};
  const uint64_t random = (value & kMantissaMask) | kExponentBits;
  return base::bit_cast<double>(random) - 1;
}

}  // namespace

TimeClamper::TimeClamper() : secret_(base::RandUint64()) {}

TimeClamper::TimeClamper(uint64_t secret) : secret_(secret) {}

TimeClamper::~TimeClamper() = default;

base::TimeDelta TimeClamper::ClampTimeResolution(
    base::TimeDelta time,
    bool cross_origin_isolated_capability) const {
  int64_t time_microseconds = time.InMicroseconds();
  bool was_negative = false;
  if (time_microseconds < 0) {
    was_negative = true;
    time_microseconds =
        -std::max(-std::numeric_limits<int64_t>::max(), time_microseconds);
  }
  // The lower digits are clamped and the upper ones added back, so that large
  // numbers do not distort the uniformity.
  const int64_t time_lower_digits = time_microseconds % kTenLowerDigitsMod;
  int64_t time_upper_digits = time_microseconds - time_lower_digits;
  const int resolution = cross_origin_isolated_capability
                             ? kFineResolutionMicroseconds
                             : kCoarseResolutionMicroseconds;
  int64_t clamped_time = time_lower_digits - time_lower_digits % resolution;
  // Clamped up, rather than down, with a probability proportional to the
  // distance from the clamped-down time.
  if (time_lower_digits >= ThresholdFor(clamped_time, resolution)) {
    clamped_time += resolution;
  }
  time_upper_digits = std::min(
      time_upper_digits, std::numeric_limits<int64_t>::max() - clamped_time);
  clamped_time += time_upper_digits;
  if (was_negative) {
    clamped_time = -clamped_time;
  }
  return base::Microseconds(clamped_time);
}

double TimeClamper::ThresholdFor(int64_t clamped_time, int resolution) const {
  const uint64_t time_hash =
      MurmurHash3(static_cast<uint64_t>(clamped_time) ^ secret_);
  return clamped_time + resolution * ToDouble(time_hash);
}

}  // namespace mitto
