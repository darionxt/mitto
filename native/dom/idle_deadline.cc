#include "mitto/dom/idle_deadline.h"

#include "mitto/bindings/v8_idle_deadline.h"
#include "mitto/dom/performance.h"

namespace mitto {

IdleDeadline::IdleDeadline(base::TimeTicks deadline, bool did_timeout)
    : deadline_(deadline), did_timeout_(did_timeout) {}

IdleDeadline::~IdleDeadline() = default;

const bindings::WrapperTypeInfo* IdleDeadline::GetWrapperTypeInfo() const {
  return &bindings::V8IdleDeadline::kWrapperTypeInfo;
}

double IdleDeadline::TimeRemaining() const {
  // A Page has no input or frames, so no high priority work makes it yield.
  const base::TimeDelta time_remaining = deadline_ - base::TimeTicks::Now();
  if (time_remaining.is_negative()) {
    return 0;
  }
  return Performance::ClampTimeResolution(time_remaining);
}

}  // namespace mitto
