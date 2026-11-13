#include "mitto/page/idle_helper.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"

namespace mitto {
namespace {

using base::sequence_manager::TaskQueue;

// IdleHelper's kMaximumIdlePeriodDuration, kMinimumIdlePeriodDuration and
// kRetryEnableLongIdlePeriodDelay.
constexpr base::TimeDelta kMaximumIdlePeriodDuration = base::Milliseconds(50);
constexpr base::TimeDelta kMinimumIdlePeriodDuration = base::Milliseconds(1);
constexpr base::TimeDelta kRetryEnableLongIdlePeriodDelay =
    base::Milliseconds(1);

}  // namespace

IdleHelper::IdleHelper(
    base::sequence_manager::SequenceManager* sequence_manager,
    scoped_refptr<base::SingleThreadTaskRunner> control_runner,
    TaskQueue* idle_queue)
    : sequence_manager_(sequence_manager),
      control_runner_(std::move(control_runner)),
      idle_queue_(idle_queue),
      idle_runner_(idle_queue->task_runner()) {
  enable_next_long_idle_period_closure_.Reset(base::BindRepeating(
      &IdleHelper::EnableLongIdlePeriod, weak_factory_.GetWeakPtr()));
  // This fence will block any idle tasks from running.
  idle_queue_->InsertFence(TaskQueue::InsertFencePosition::kBeginningOfTime);
}

IdleHelper::~IdleHelper() {
  EndIdlePeriod();
}

IdleHelper::IdlePeriodState IdleHelper::ComputeNewLongIdlePeriodState(
    const base::TimeTicks now,
    base::TimeDelta* next_long_idle_period_delay_out) {
  // MainThreadSchedulerImpl::CanEnterLongIdlePeriod() refuses only while a
  // touch starts, which no Page has.
  const std::optional<base::sequence_manager::WakeUp> wake_up =
      sequence_manager_->GetNextDelayedWakeUp();
  base::TimeDelta long_idle_period_duration;
  if (wake_up) {
    // Limit the idle period duration to be before the next pending task.
    long_idle_period_duration =
        std::min(wake_up->time - now, kMaximumIdlePeriodDuration);
  } else {
    long_idle_period_duration = kMaximumIdlePeriodDuration;
  }
  if (long_idle_period_duration >= kMinimumIdlePeriodDuration) {
    *next_long_idle_period_delay_out = long_idle_period_duration;
    if (!idle_queue_->HasTaskToRunImmediatelyOrReadyDelayedTask()) {
      return IdlePeriodState::kInLongIdlePeriodPaused;
    }
    return IdlePeriodState::kInLongIdlePeriod;
  }
  // If we can't start the idle period yet then try again after wake-up.
  *next_long_idle_period_delay_out = kRetryEnableLongIdlePeriodDelay;
  return IdlePeriodState::kNotInIdlePeriod;
}

void IdleHelper::EnableLongIdlePeriod() {
  // End any previous idle period. The main thread waits for no quiescence
  // first.
  EndIdlePeriod();
  const base::TimeTicks now = base::TimeTicks::Now();
  base::TimeDelta next_long_idle_period_delay;
  const IdlePeriodState new_idle_period_state =
      ComputeNewLongIdlePeriodState(now, &next_long_idle_period_delay);
  if (new_idle_period_state != IdlePeriodState::kNotInIdlePeriod) {
    StartIdlePeriod(new_idle_period_state, now,
                    now + next_long_idle_period_delay);
  } else {
    // Otherwise wait for the next long idle period delay before trying again.
    control_runner_->PostDelayedTask(
        FROM_HERE, enable_next_long_idle_period_closure_.callback(),
        next_long_idle_period_delay);
  }
}

void IdleHelper::StartIdlePeriod(IdlePeriodState new_state,
                                 base::TimeTicks now,
                                 base::TimeTicks idle_period_deadline) {
  DCHECK_GT(idle_period_deadline, now);
  DCHECK_NE(new_state, IdlePeriodState::kNotInIdlePeriod);
  // No idle task of the Page is delayed, so none needs to be moved in first.
  if (idle_period_deadline - now < kMinimumIdlePeriodDuration) {
    return;
  }
  if (!IsInIdlePeriod()) {
    sequence_manager_->AddTaskObserver(this);
  }
  // Use a fence to make sure any idle tasks posted after this point do not
  // run until the next idle period and unblock existing tasks.
  idle_queue_->InsertFence(TaskQueue::InsertFencePosition::kNow);
  idle_period_state_ = new_state;
  idle_period_deadline_ = idle_period_deadline;
}

void IdleHelper::EndIdlePeriod() {
  // CancelableClosureHolder::Cancel(): what was posted does nothing, and what
  // is posted next starts a period again.
  enable_next_long_idle_period_closure_.Reset(base::BindRepeating(
      &IdleHelper::EnableLongIdlePeriod, weak_factory_.GetWeakPtr()));
  // If we weren't already within an idle period then early-out.
  if (!IsInIdlePeriod()) {
    return;
  }
  sequence_manager_->RemoveTaskObserver(this);
  // This fence will block any idle tasks from running.
  idle_queue_->InsertFence(TaskQueue::InsertFencePosition::kBeginningOfTime);
  idle_period_state_ = IdlePeriodState::kNotInIdlePeriod;
  idle_period_deadline_ = base::TimeTicks();
}

void IdleHelper::WillProcessTask(const base::PendingTask& pending_task,
                                 bool was_blocked_or_low_priority) {}

void IdleHelper::DidProcessTask(const base::PendingTask& pending_task) {
  DCHECK(IsInIdlePeriod());
  if (idle_period_state_ != IdlePeriodState::kInLongIdlePeriodPaused &&
      base::TimeTicks::Now() >= idle_period_deadline_) {
    // If the idle period deadline has now been reached, trigger a new long
    // idle period.
    EnableLongIdlePeriod();
  }
}

void IdleHelper::UpdateLongIdlePeriodStateAfterIdleTask() {
  DCHECK(IsInLongIdlePeriod());
  if (!idle_queue_->HasTaskToRunImmediatelyOrReadyDelayedTask()) {
    // If there are no more idle tasks then pause long idle period ticks until
    // a new idle task is posted.
    idle_period_state_ = IdlePeriodState::kInLongIdlePeriodPaused;
  } else if (idle_queue_->BlockedByFence()) {
    // If there is still idle work to do then just start the next idle period
    // once this one ends.
    const base::TimeDelta next_long_idle_period_delay = std::max(
        base::TimeDelta(), idle_period_deadline_ - base::TimeTicks::Now());
    if (next_long_idle_period_delay.is_zero()) {
      EnableLongIdlePeriod();
    } else {
      control_runner_->PostDelayedTask(
          FROM_HERE, enable_next_long_idle_period_closure_.callback(),
          next_long_idle_period_delay);
    }
  }
}

void IdleHelper::PostIdleTask(const base::Location& from_here,
                              EventLoop::IdleTask task) {
  // IdleHelper::OnIdleTaskPostedOnMainThread(): restart long idle period
  // ticks.
  if (idle_period_state_ == IdlePeriodState::kInLongIdlePeriodPaused) {
    control_runner_->PostTask(FROM_HERE,
                              enable_next_long_idle_period_closure_.callback());
  }
  idle_runner_->PostTask(
      from_here, base::BindOnce(&IdleHelper::RunTask,
                                weak_factory_.GetWeakPtr(), std::move(task)));
}

bool IdleHelper::IsInIdlePeriod() const {
  return idle_period_state_ != IdlePeriodState::kNotInIdlePeriod;
}

bool IdleHelper::IsInLongIdlePeriod() const {
  return idle_period_state_ == IdlePeriodState::kInLongIdlePeriod ||
         idle_period_state_ == IdlePeriodState::kInLongIdlePeriodPaused;
}

// SingleThreadIdleTaskRunner::RunTask(), with IdleHelper's
// WillProcessIdleTask() and DidProcessIdleTask().
void IdleHelper::RunTask(EventLoop::IdleTask task) {
  const base::WeakPtr<IdleHelper> self = weak_factory_.GetWeakPtr();
  std::move(task).Run(idle_period_deadline_);
  // The task may have ended the Page's main thread, and this with it.
  if (self && IsInLongIdlePeriod()) {
    UpdateLongIdlePeriodStateAfterIdleTask();
  }
}

}  // namespace mitto
