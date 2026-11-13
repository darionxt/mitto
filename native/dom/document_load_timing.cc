#include "mitto/dom/document_load_timing.h"

#include "mitto/dom/performance.h"

namespace mitto {

ResourceTimingInfo::ResourceTimingInfo() = default;
ResourceTimingInfo::ResourceTimingInfo(const ResourceTimingInfo&) = default;
ResourceTimingInfo& ResourceTimingInfo::operator=(const ResourceTimingInfo&) =
    default;
ResourceTimingInfo::ResourceTimingInfo(ResourceTimingInfo&&) = default;
ResourceTimingInfo& ResourceTimingInfo::operator=(ResourceTimingInfo&&) =
    default;
ResourceTimingInfo::~ResourceTimingInfo() = default;

DocumentLoadTiming::DocumentLoadTiming(base::TimeTicks start)
    : navigation_start(start),
      // DocumentLoadTiming::EnsureReferenceTimesSet() and
      // SetNavigationStart().
      unix_at_zero_monotonic_(base::Time::Now() -
                              (base::TimeTicks::Now() - base::TimeTicks())) {}

DocumentLoadTiming::~DocumentLoadTiming() = default;

uint64_t DocumentLoadTiming::ToIntegerMilliseconds(base::TimeTicks time) const {
  // DocumentLoadTiming::MonotonicTimeToPseudoWallTime(), then
  // PerformanceTiming's ToIntegerMilliseconds(), which coarsens with the same
  // clamper as performance.timeOrigin, so that the two agree.
  // Upstream: third_party/blink/renderer/core/timing/performance_timing.cc
  if (time.is_null()) {
    return 0;
  }
  const base::Time wall = unix_at_zero_monotonic_ + (time - base::TimeTicks());
  return static_cast<uint64_t>(
      Performance::ClampTimeResolution(wall - base::Time::UnixEpoch()));
}

}  // namespace mitto
