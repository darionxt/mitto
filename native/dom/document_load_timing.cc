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

void DocumentLoadTiming::Restart(base::TimeTicks start) {
  // Every field but the reference times.
  navigation_start = start;
  fetch_start = base::TimeTicks();
  redirect_start = base::TimeTicks();
  redirect_end = base::TimeTicks();
  redirect_count = 0;
  has_cross_origin_redirect = false;
  response_end = base::TimeTicks();
  load_event_start = base::TimeTicks();
  load_event_end = base::TimeTicks();
  critical_ch_restart = base::TimeTicks();
  is_reload = false;
  unload_event_start = base::TimeTicks();
  unload_event_end = base::TimeTicks();
  has_same_origin_as_previous_document = false;
  high_confidence.reset();
  dom_loading = base::TimeTicks();
  dom_interactive = base::TimeTicks();
  dom_content_loaded_event_start = base::TimeTicks();
  dom_content_loaded_event_end = base::TimeTicks();
  dom_complete = base::TimeTicks();
  has_response = false;
  navigation = ResourceTimingInfo();
  connection_info = "unknown";
  alpn_negotiated_protocol = "unknown";
  was_fetched_via_spdy = false;
  was_alpn_negotiated = false;
}

uint64_t DocumentLoadTiming::ToIntegerMilliseconds(base::TimeTicks time) const {
  // DocumentLoadTiming::MonotonicTimeToPseudoWallTime(), then
  // PerformanceTiming's ToIntegerMilliseconds(), which coarsens with the same
  // clamper as performance.timeOrigin, so that the two agree.
  // Upstream: third_party/blink/renderer/core/timing/performance_timing.cc
  if (time.is_null()) {
    return 0;
  }
  return static_cast<uint64_t>(
      Performance::ClampTimeResolution(MonotonicTimeToPseudoWallTime(time)));
}

base::TimeDelta DocumentLoadTiming::MonotonicTimeToPseudoWallTime(
    base::TimeTicks time) const {
  return unix_at_zero_monotonic_ + (time - base::TimeTicks()) -
         base::Time::UnixEpoch();
}

}  // namespace mitto
