// The idle periods of the Page's main thread, as Blink's IdleHelper runs them:
// while the Page begins no frames, long idle periods, each of which lasts until
// the next delayed task is due, and at most 50 milliseconds, and between the
// frames that it begins, short idle periods that end when the next frame is
// due. Idle tasks wait in a queue of their own, behind a fence that lets
// through only those posted before the period started, and each gets the
// period's end. Once they have run in a long idle period, the next period
// starts when this one ends if more wait, and periods pause until one is
// posted if none does. A short idle period just ends.
//
// Upstream: third_party/blink/renderer/platform/scheduler/common/idle_helper.cc
// Upstream: third_party/blink/renderer/platform/scheduler/common/single_thread_idle_task_runner.cc

#ifndef MITTO_PAGE_IDLE_HELPER_H_
#define MITTO_PAGE_IDLE_HELPER_H_

#include "base/cancelable_callback.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/task/sequence_manager/task_queue.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_observer.h"
#include "base/time/time.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

class IdleHelper final : public base::TaskObserver {
 public:
  // Runs idle tasks on `idle_queue` and its own tasks on `control_runner`'s
  // queue, which must come first, in `sequence_manager`. All of them must
  // outlive it.
  IdleHelper(base::sequence_manager::SequenceManager* sequence_manager,
             scoped_refptr<base::SingleThreadTaskRunner> control_runner,
             base::sequence_manager::TaskQueue* idle_queue);

  IdleHelper(const IdleHelper&) = delete;
  IdleHelper& operator=(const IdleHelper&) = delete;

  ~IdleHelper() override;

  // Ends the current idle period, if one has started, and starts a long one
  // if it can, or tries again once it might.
  void EnableLongIdlePeriod();
  // Starts an idle period that ends at `idle_period_deadline`, as between two
  // frames, unless it would be shorter than a millisecond.
  void StartShortIdlePeriod(base::TimeTicks now,
                            base::TimeTicks idle_period_deadline);
  // Ends the current idle period, if one has started, as a frame begins.
  void EndIdlePeriod();

  void PostIdleTask(const base::Location& from_here, EventLoop::IdleTask task);

  // base::TaskObserver:
  void WillProcessTask(const base::PendingTask& pending_task,
                       bool was_blocked_or_low_priority) override;
  void DidProcessTask(const base::PendingTask& pending_task) override;

 private:
  enum class IdlePeriodState {
    kNotInIdlePeriod,
    kInShortIdlePeriod,
    kInLongIdlePeriod,
    // A long idle period with no idle task to run, which ends only when one
    // is posted.
    kInLongIdlePeriodPaused,
  };

  // The state a long idle period that started now would have, and how long
  // it would last or when to try again.
  IdlePeriodState ComputeNewLongIdlePeriodState(
      base::TimeTicks now,
      base::TimeDelta* next_long_idle_period_delay_out);
  void StartIdlePeriod(IdlePeriodState new_state,
                       base::TimeTicks now,
                       base::TimeTicks idle_period_deadline);
  void UpdateLongIdlePeriodStateAfterIdleTask();
  bool IsInIdlePeriod() const;
  bool IsInLongIdlePeriod() const;
  void RunTask(EventLoop::IdleTask task);

  const raw_ptr<base::sequence_manager::SequenceManager> sequence_manager_;
  const scoped_refptr<base::SingleThreadTaskRunner> control_runner_;
  const raw_ptr<base::sequence_manager::TaskQueue> idle_queue_;
  const scoped_refptr<base::SingleThreadTaskRunner> idle_runner_;
  IdlePeriodState idle_period_state_ = IdlePeriodState::kNotInIdlePeriod;
  base::TimeTicks idle_period_deadline_;
  base::CancelableRepeatingClosure enable_next_long_idle_period_closure_;
  base::WeakPtrFactory<IdleHelper> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_IDLE_HELPER_H_
