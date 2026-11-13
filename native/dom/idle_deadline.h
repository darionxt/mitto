// requestIdleCallback()'s IdleDeadline: how long the idle period that runs a
// callback has left, coarsened as every time that a page reads is, and
// whether the callback ran for its timeout instead.
//
// Upstream: third_party/blink/renderer/core/scheduler/idle_deadline.cc

#ifndef MITTO_DOM_IDLE_DEADLINE_H_
#define MITTO_DOM_IDLE_DEADLINE_H_

#include "base/time/time.h"
#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class IdleDeadline final : public bindings::ScriptWrappable {
 public:
  IdleDeadline(base::TimeTicks deadline, bool did_timeout);
  ~IdleDeadline() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double TimeRemaining() const;
  bool DidTimeout() const { return did_timeout_; }

 private:
  const base::TimeTicks deadline_;
  const bool did_timeout_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDLE_DEADLINE_H_
