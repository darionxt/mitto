#include "mitto/page/page_scheduler.h"

#include <utility>

#include "base/check.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/task/sequence_manager/sequence_manager_impl.h"

namespace mitto {
namespace {

using base::sequence_manager::QueueName;
using base::sequence_manager::SequenceManager;
using base::sequence_manager::TaskQueue;

// Blink's TaskPriority, highest first.
enum class TaskPriority : TaskQueue::QueuePriority {
  kControlPriority = 0,
  kHighestPriority = 1,
  kExtremelyHighPriority = 2,
  kVeryHighPriority = 3,
  kHighPriorityContinuation = 4,
  kHighPriority = 5,
  kNormalPriorityContinuation = 6,
  kNormalPriority = 7,
  kLowPriorityContinuation = 8,
  kLowPriority = 9,
  kBestEffortPriority = 10,
  kPriorityCount = 11,
};

// FrameSchedulerImpl::ComputePriority() for each queue.
TaskPriority PriorityOf(EventLoop::Queue queue) {
  switch (queue) {
    case EventLoop::Queue::kDefault:
    case EventLoop::Queue::kUserVisible:
      return TaskPriority::kNormalPriority;
    case EventLoop::Queue::kUserBlocking:
      return TaskPriority::kHighPriority;
    case EventLoop::Queue::kUserBlockingContinuation:
      return TaskPriority::kHighPriorityContinuation;
    case EventLoop::Queue::kUserVisibleContinuation:
      return TaskPriority::kNormalPriorityContinuation;
    case EventLoop::Queue::kBackground:
      return TaskPriority::kLowPriority;
    case EventLoop::Queue::kBackgroundContinuation:
      return TaskPriority::kLowPriorityContinuation;
  }
}

QueueName NameOf(EventLoop::Queue queue) {
  return queue == EventLoop::Queue::kDefault ? QueueName::DEFAULT_TQ
                                             : QueueName::WEB_SCHEDULING_TQ;
}

}  // namespace

PageScheduler::PageScheduler() {
  // CreatePrioritySettings(): Blink's priorities, the normal one by default.
  SequenceManager::PrioritySettings priorities(
      TaskPriority::kPriorityCount, TaskPriority::kNormalPriority);
  sequence_manager_ = base::sequence_manager::CreateUnboundSequenceManager(
      SequenceManager::Settings::Builder()
          .SetMessagePumpType(base::MessagePumpType::DEFAULT)
          .SetIsMainThread(true)
          .SetPrioritySettings(std::move(priorities))
          .Build());
  for (size_t i = 0; i < kQueueCount; ++i) {
    const Queue queue = static_cast<Queue>(i);
    queues_[i] = sequence_manager_->CreateTaskQueue(
        base::sequence_manager::TaskQueue::Spec(NameOf(queue)));
    queues_[i]->SetQueuePriority(PriorityOf(queue));
  }
  control_queue_ = sequence_manager_->CreateTaskQueue(
      TaskQueue::Spec(QueueName::CONTROL_TQ));
  control_queue_->SetQueuePriority(TaskPriority::kControlPriority);
  compositor_queue_ = sequence_manager_->CreateTaskQueue(
      TaskQueue::Spec(QueueName::COMPOSITOR_TQ));
  idle_queue_ =
      sequence_manager_->CreateTaskQueue(TaskQueue::Spec(QueueName::IDLE_TQ));
  idle_queue_->SetQueuePriority(TaskPriority::kBestEffortPriority);
  sequence_manager_->SetDefaultTaskQueue(
      queues_[static_cast<size_t>(Queue::kDefault)].get());
  sequence_manager_->BindToMessagePump(
      base::MessagePump::Create(base::MessagePumpType::DEFAULT));
  idle_helper_ = std::make_unique<IdleHelper>(sequence_manager_.get(),
                                              control_queue_->task_runner(),
                                              idle_queue_.get());
  // MainThreadSchedulerImpl::BeginFrameNotExpectedSoon(): a page that draws
  // no frames has long idle periods from the start.
  idle_helper_->EnableLongIdlePeriod();
  // ComputeCompositorPriority(): the normal priority, and a very high one
  // while rendering starves.
  begin_frame_source_ = std::make_unique<BeginFrameSource>(
      sequence_manager_.get(), compositor_queue_.get(),
      BeginFrameSource::Priorities{
          .normal = static_cast<TaskQueue::QueuePriority>(
              TaskPriority::kNormalPriority),
          .starved = static_cast<TaskQueue::QueuePriority>(
              TaskPriority::kVeryHighPriority)},
      idle_helper_.get());
  CHECK(!EventLoop::Get());
  EventLoop::Set(this);
}

PageScheduler::~PageScheduler() {
  EventLoop::Set(nullptr);
  begin_frame_source_.reset();
  idle_helper_.reset();
}

const scoped_refptr<base::SingleThreadTaskRunner>& PageScheduler::task_runner()
    const {
  return queues_[static_cast<size_t>(Queue::kDefault)]->task_runner();
}

scoped_refptr<base::SingleThreadTaskRunner> PageScheduler::TaskRunner(
    Queue queue) {
  return queues_[static_cast<size_t>(queue)]->task_runner();
}

void PageScheduler::PostIdleTask(const base::Location& from_here,
                                 IdleTask task) {
  idle_helper_->PostIdleTask(from_here, std::move(task));
}

void PageScheduler::RequestBeginFrame(BeginFrameCallback begin_frame) {
  begin_frame_source_->RequestBeginFrame(std::move(begin_frame));
}

void PageScheduler::SetRefreshRate(int hertz) {
  begin_frame_source_->SetRefreshRate(hertz);
}

}  // namespace mitto
