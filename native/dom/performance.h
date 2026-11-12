// The High Resolution Time standard's Performance, a window's `performance`,
// with now() and timeOrigin, coarsened as Chrome coarsens them, and Blink's
// memory. Its timelines, marks and measures are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/timing/performance.h
// Upstream: third_party/blink/renderer/core/timing/performance.cc
// Upstream: third_party/blink/renderer/core/timing/window_performance.cc

#ifndef MITTO_DOM_PERFORMANCE_H_
#define MITTO_DOM_PERFORMANCE_H_

#include "base/time/time.h"
#include "mitto/dom/event_target.h"

namespace mitto {

class MemoryInfo;

class Performance final : public EventTarget {
 public:
  // Counts from `time_origin`: the start of the navigation that loaded the
  // window's document, or when the window was made.
  static Performance* Create(base::TimeTicks time_origin);

  explicit Performance(base::TimeTicks time_origin);
  ~Performance() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double Now() const;
  double TimeOrigin() const;
  // WindowPerformance::memory(): a new MemoryInfo for each call, precise while
  // the process is locked to a site.
  MemoryInfo* Memory() const;

  // Blink's MonotonicTimeToDOMHighResTimeStamp(): `time` in milliseconds since
  // `time_origin`, each coarsened before the difference, and 0 for a time
  // before it.
  static double MonotonicTimeToDOMHighResTimeStamp(base::TimeTicks time_origin,
                                                   base::TimeTicks time);

 private:
  const base::TimeTicks time_origin_;
  // The Unix time at the zero of base::TimeTicks, from which timeOrigin is
  // given as a Unix time.
  const base::Time unix_at_zero_monotonic_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_H_
