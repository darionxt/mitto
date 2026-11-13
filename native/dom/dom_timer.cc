#include "mitto/dom/dom_timer.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/numerics/clamped_math.h"
#include "base/numerics/safe_conversions.h"
#include "base/timer/timer.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {
namespace {

// Chrome increments the nesting level before it compares it, contrary to the
// order of the HTML Standard's steps (crbug.com/1108877), and compares it
// with 6, which gives the standard's outcome: the seventh timer of a chain is
// the first that is clamped.
constexpr int kMaxTimerNestingLevel = 6;
constexpr base::TimeDelta kMinimumInterval = base::Milliseconds(4);

// Timers shorter than this run on time, and longer ones when the thread wakes
// up anyway soon after, as Chrome has it with neither its lower threshold nor
// //base's aligned wake-ups on, as on stable by default.
constexpr base::TimeDelta kMaxHighResolutionInterval = base::Milliseconds(32);

}  // namespace

TimerSchedule ScheduleTimer(int nesting_level, base::TimeDelta timeout) {
  TimerSchedule schedule;
  // Step 10.
  if (timeout.is_negative()) {
    timeout = base::TimeDelta();
  }
  // Steps 12 and 13, before step 11, as in Chrome.
  schedule.nesting_level = base::ClampAdd(nesting_level, 1);
  schedule.precise = timeout < kMaxHighResolutionInterval;
  // Step 11.
  if (schedule.nesting_level > kMaxTimerNestingLevel &&
      timeout < kMinimumInterval) {
    timeout = kMinimumInterval;
  }
  // No clamp to 1 millisecond: setTimeout() has none, and setInterval() none
  // since SetIntervalWithoutClamp, which stable has on.
  schedule.delay = timeout;
  return schedule;
}

base::TimeDelta RepeatIntervalAt(int nesting_level, base::TimeDelta interval) {
  if (nesting_level == kMaxTimerNestingLevel + 1 &&
      interval < kMinimumInterval) {
    return kMinimumInterval;
  }
  return interval;
}

struct DOMTimerCoordinator::Action {
  v8::Global<v8::Function> function;
  std::vector<v8::Global<v8::Value>> arguments;
  std::u16string code;
};

// One timer, and the //base timer that runs its task.
class DOMTimerCoordinator::DOMTimer {
 public:
  DOMTimer(DOMTimerCoordinator& coordinator,
           int32_t id,
           std::unique_ptr<Action> action,
           const TimerSchedule& schedule,
           bool single_shot)
      : coordinator_(coordinator),
        id_(id),
        action_(std::move(action)),
        nesting_level_(schedule.nesting_level),
        delay_policy_(schedule.precise
                          ? base::subtle::DelayPolicy::kPrecise
                          : base::subtle::DelayPolicy::kFlexibleNoSooner) {
    if (!single_shot) {
      repeat_interval_ = schedule.delay;
    }
    // TimerBase::Start(): no delay is a null time, for which the task is an
    // immediate one, ordered with the tasks posted beside it, not a delayed
    // one ordered once its time comes.
    next_fire_time_ = schedule.delay.is_zero()
                          ? base::TimeTicks()
                          : base::TimeTicks::Now() + schedule.delay;
    Arm();
  }

  DOMTimer(const DOMTimer&) = delete;
  DOMTimer& operator=(const DOMTimer&) = delete;

  ~DOMTimer() = default;

 private:
  void Arm() {
    timer_.Start(FROM_HERE, next_fire_time_,
                 base::BindOnce(&DOMTimer::Fired, base::Unretained(this)),
                 delay_policy_);
  }

  // Blink's TimerBase::RunInternal() and DOMTimer::Fired().
  void Fired() {
    DOMTimerCoordinator& coordinator = *coordinator_;
    coordinator.timer_nesting_level_ = nesting_level_;
    if (repeat_interval_) {
      // The next run is on the grid of the interval, past a twentieth of it
      // from now, so that a late wake-up does not run it twice to catch up.
      const base::TimeTicks now = base::TimeTicks::Now();
      if (repeat_interval_->is_zero()) {
        next_fire_time_ = base::TimeTicks();
      } else {
        next_fire_time_ =
            (now + *repeat_interval_ / 20)
                .SnappedToNextTick(next_fire_time_, *repeat_interval_);
      }
      nesting_level_ = base::ClampAdd(nesting_level_, 1);
      const base::TimeDelta interval =
          RepeatIntervalAt(nesting_level_, *repeat_interval_);
      // DOMTimer::Fired() and TimerBase::AugmentRepeatInterval(): a clamped
      // interval moves the next run on, but not before now.
      if (interval != *repeat_interval_) {
        next_fire_time_ =
            std::max(now, next_fire_time_ + (interval - *repeat_interval_));
      }
      repeat_interval_ = interval;
      Arm();
      // The action may clear this timer, which then waits in `clearing_`.
      coordinator.Execute(*action_);
      coordinator.timer_nesting_level_ = 0;
      coordinator.clearing_.clear();
      return;
    }
    // A one-shot timer leaves the list before its action runs.
    std::unique_ptr<DOMTimer> self = coordinator.Take(id_);
    coordinator.Execute(*action_);
    coordinator.timer_nesting_level_ = 0;
    coordinator.clearing_.clear();
  }

  const raw_ref<DOMTimerCoordinator> coordinator_;
  const int32_t id_;
  const std::unique_ptr<Action> action_;
  int nesting_level_;
  const base::subtle::DelayPolicy delay_policy_;
  std::optional<base::TimeDelta> repeat_interval_;
  base::TimeTicks next_fire_time_;
  base::DeadlineTimer timer_;
};

DOMTimerCoordinator::DOMTimerCoordinator(v8::Isolate* isolate,
                                         v8::Local<v8::Context> context)
    : isolate_(isolate),
      context_(std::make_unique<v8::Global<v8::Context>>(isolate, context)) {}

DOMTimerCoordinator::~DOMTimerCoordinator() = default;

int32_t DOMTimerCoordinator::SetTimeout(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  auto action = std::make_unique<Action>();
  action->function.Reset(isolate_, handler);
  for (const v8::Local<v8::Value> argument : arguments) {
    action->arguments.emplace_back(isolate_, argument);
  }
  return Install(std::move(action), timeout, /*single_shot=*/true);
}

int32_t DOMTimerCoordinator::SetTimeout(const std::u16string& handler,
                                        int32_t timeout) {
  // Chrome sets no timer to run nothing.
  if (handler.empty()) {
    return 0;
  }
  auto action = std::make_unique<Action>();
  action->code = handler;
  return Install(std::move(action), timeout, /*single_shot=*/true);
}

int32_t DOMTimerCoordinator::SetInterval(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  auto action = std::make_unique<Action>();
  action->function.Reset(isolate_, handler);
  for (const v8::Local<v8::Value> argument : arguments) {
    action->arguments.emplace_back(isolate_, argument);
  }
  return Install(std::move(action), timeout, /*single_shot=*/false);
}

int32_t DOMTimerCoordinator::SetInterval(const std::u16string& handler,
                                         int32_t timeout) {
  if (handler.empty()) {
    return 0;
  }
  auto action = std::make_unique<Action>();
  action->code = handler;
  return Install(std::move(action), timeout, /*single_shot=*/false);
}

void DOMTimerCoordinator::Clear(int32_t id) {
  if (id <= 0) {
    return;
  }
  std::unique_ptr<DOMTimer> timer = Take(id);
  // A repeating timer that clears itself goes once its action returns.
  if (timer && timer_nesting_level_ != 0) {
    clearing_.push_back(std::move(timer));
  }
}

int32_t DOMTimerCoordinator::Install(std::unique_ptr<Action> action,
                                     int32_t timeout,
                                     bool single_shot) {
  const int32_t id = NextId();
  const TimerSchedule schedule =
      ScheduleTimer(timer_nesting_level_, base::Milliseconds(timeout));
  timers_.emplace(id, std::make_unique<DOMTimer>(*this, id, std::move(action),
                                                 schedule, single_shot));
  return id;
}

int32_t DOMTimerCoordinator::NextId() {
  while (true) {
    if (circular_sequential_id_ == std::numeric_limits<int32_t>::max()) {
      circular_sequential_id_ = 1;
    } else {
      ++circular_sequential_id_;
    }
    if (!timers_.contains(circular_sequential_id_)) {
      return circular_sequential_id_;
    }
  }
}

std::unique_ptr<DOMTimerCoordinator::DOMTimer> DOMTimerCoordinator::Take(
    int32_t id) {
  const auto found = timers_.find(id);
  if (found == timers_.end()) {
    return nullptr;
  }
  std::unique_ptr<DOMTimer> timer = std::move(found->second);
  timers_.erase(found);
  return timer;
}

// Blink's ScheduledAction::Execute(): a function is called with the window as
// `this` and the timer's arguments; source text runs as a classic script. What
// either throws is reported, as an error that nothing caught.
void DOMTimerCoordinator::Execute(const Action& action) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_->Get(isolate_);
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate_);
  if (!action.function.IsEmpty()) {
    std::vector<v8::Local<v8::Value>> arguments;
    arguments.reserve(action.arguments.size());
    for (const v8::Global<v8::Value>& argument : action.arguments) {
      arguments.push_back(argument.Get(isolate_));
    }
    std::ignore = action.function.Get(isolate_)->Call(
        context, context->Global(), base::checked_cast<int>(arguments.size()),
        arguments.data());
  } else {
    v8::Local<v8::String> source;
    v8::Local<v8::Script> script;
    if (action.code.size() <= static_cast<size_t>(v8::String::kMaxLength) &&
        v8::String::NewFromTwoByte(
            isolate_, reinterpret_cast<const uint16_t*>(action.code.data()),
            v8::NewStringType::kNormal,
            base::checked_cast<int>(action.code.size()))
            .ToLocal(&source) &&
        v8::Script::Compile(context, source).ToLocal(&script)) {
      std::ignore = script->Run(context);
    }
  }
  if (try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate_, try_catch.Exception());
  }
}

}  // namespace mitto
