#include "mitto/dom/performance.h"

#include "base/no_destructor.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_performance.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/time_clamper.h"

namespace mitto {
namespace {

// A Page is never cross-origin isolated.
constexpr bool kCrossOriginIsolated = false;

// Blink's GetUnixAtZeroMonotonic().
base::Time UnixAtZeroMonotonic() {
  return base::Time::Now() - (base::TimeTicks::Now() - base::TimeTicks());
}

}  // namespace

// static
Performance* Performance::Create(base::TimeTicks time_origin) {
  return bindings::MakeGarbageCollected<Performance>(time_origin);
}

Performance::Performance(base::TimeTicks time_origin)
    : time_origin_(time_origin),
      unix_at_zero_monotonic_(UnixAtZeroMonotonic()) {}

Performance::~Performance() = default;

const bindings::WrapperTypeInfo* Performance::GetWrapperTypeInfo() const {
  return &bindings::V8Performance::kWrapperTypeInfo;
}

double Performance::Now() const {
  return MonotonicTimeToDOMHighResTimeStamp(time_origin_,
                                            base::TimeTicks::Now());
}

double Performance::TimeOrigin() const {
  return ClampTimeResolution(
      (unix_at_zero_monotonic_ - base::Time::UnixEpoch()) +
      (time_origin_ - base::TimeTicks()));
}

MemoryInfo* Performance::Memory() const {
  return MemoryInfo::Create(MemoryInfo::IsProcessLockedToSite()
                                ? MemoryInfo::Precision::kPrecise
                                : MemoryInfo::Precision::kBucketized);
}

// static
double Performance::ClampTimeResolution(base::TimeDelta time) {
  // One secret for the process, as Blink keeps one static clamper.
  static const base::NoDestructor<TimeClamper> clamper;
  return clamper->ClampTimeResolution(time, kCrossOriginIsolated)
      .InMillisecondsF();
}

// static
double Performance::MonotonicTimeToDOMHighResTimeStamp(
    base::TimeTicks time_origin,
    base::TimeTicks time) {
  // No raw timestamp of the platform's goes out.
  if (time.is_null() || time_origin.is_null()) {
    return 0.0;
  }
  const double clamped_time = ClampTimeResolution(time.since_origin()) -
                              ClampTimeResolution(time_origin.since_origin());
  return clamped_time < 0 ? 0.0 : clamped_time;
}

}  // namespace mitto
