// The timers of a window: setTimeout(), setInterval() and the clearing of
// either, as the HTML Standard has them, and as Chrome runs them where it
// differs: on the real clock, as delayed tasks of the Page's event loop.
//
// Upstream: third_party/blink/renderer/core/scheduler/dom_timer.h
// Upstream: third_party/blink/renderer/core/scheduler/dom_timer.cc
// Upstream: third_party/blink/renderer/core/scheduler/scheduled_action.cc
// Upstream: third_party/blink/renderer/platform/timer.cc

#ifndef MITTO_DOM_DOM_TIMER_H_
#define MITTO_DOM_DOM_TIMER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "v8/include/v8-forward.h"

namespace mitto {

// When a timer runs, as Chrome decides it from the HTML Standard's timer
// initialisation steps.
struct TimerSchedule {
  // How long after it is set it runs, after clamping.
  base::TimeDelta delay;
  // The timer's nesting level, which the timers that its task sets start from.
  int nesting_level = 0;
  // Whether it runs on time, rather than when the thread wakes up anyway soon
  // after.
  bool precise = false;
};

// The schedule of a timer set with `timeout` while a timer of `nesting_level`
// runs, or 0 outside a timer.
TimerSchedule ScheduleTimer(int nesting_level, base::TimeDelta timeout);

// The interval of a repeating timer whose nesting level has just become
// `nesting_level`, which was `interval`: at least 4 milliseconds once it is
// deep enough.
base::TimeDelta RepeatIntervalAt(int nesting_level, base::TimeDelta interval);

// The timers of one window's context. It stops each when it goes, as Blink
// stops a context's timers when the context is destroyed.
class DOMTimerCoordinator {
 public:
  // The timers run in `context`.
  DOMTimerCoordinator(v8::Isolate* isolate, v8::Local<v8::Context> context);

  DOMTimerCoordinator(const DOMTimerCoordinator&) = delete;
  DOMTimerCoordinator& operator=(const DOMTimerCoordinator&) = delete;

  ~DOMTimerCoordinator();

  // Each returns the timer's ID, which is never 0, or 0 for a timer that it
  // does not set: one whose handler is an empty string, as in Chrome.
  int32_t SetTimeout(v8::Local<v8::Function> handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetTimeout(const std::u16string& handler, int32_t timeout);
  int32_t SetInterval(v8::Local<v8::Function> handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetInterval(const std::u16string& handler, int32_t timeout);

  // Stops the timer with ID `id`, if there is one, whichever kind it is.
  void Clear(int32_t id);

  // How many timers are set.
  size_t size() const { return timers_.size(); }

 private:
  class DOMTimer;
  friend class DOMTimer;

  // A timer's handler: a function and its arguments, or source text.
  struct Action;

  int32_t Install(std::unique_ptr<Action> action,
                  int32_t timeout,
                  bool single_shot);
  int32_t NextId();
  // Takes the timer with ID `id` out of the list, if there is one.
  std::unique_ptr<DOMTimer> Take(int32_t id);
  void Execute(const Action& action);

  const raw_ptr<v8::Isolate> isolate_;
  const std::unique_ptr<v8::Global<v8::Context>> context_;
  std::map<int32_t, std::unique_ptr<DOMTimer>> timers_;
  int32_t circular_sequential_id_ = 0;
  // The nesting level of the timer that runs, or 0.
  int timer_nesting_level_ = 0;
  // The timers cleared while a timer runs, which go once it returns, since the
  // one that runs may be among them.
  std::vector<std::unique_ptr<DOMTimer>> clearing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_TIMER_H_
