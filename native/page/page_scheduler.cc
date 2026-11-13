#include "mitto/page/page_scheduler.h"

#include <utility>

#include "base/check.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/notreached.h"
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

// FrameSchedulerImpl::CreateQueueTraitsForTaskType(): the queue of each task
// type, by its traits.
PageScheduler::FrameQueue QueueOf(TaskType type) {
  using FrameQueue = PageScheduler::FrameQueue;
  switch (type) {
    case TaskType::kJavascriptTimerDelayedLowNesting:
    case TaskType::kJavascriptTimerDelayedHighNesting:
    case TaskType::kIdleTask:
      return FrameQueue::kThrottleable;
    case TaskType::kInternalLoading:
    case TaskType::kNetworking:
    case TaskType::kNetworkingUnfreezable:
      return FrameQueue::kLoading;
    case TaskType::kNetworkingUnfreezableRenderBlockingLoading:
      return FrameQueue::kRenderBlockingLoading;
    case TaskType::kNetworkingControl:
      return FrameQueue::kLoadingControl;
    case TaskType::kLowPriorityScriptExecution:
      return FrameQueue::kLowPriorityScriptExecution;
    case TaskType::kJavascriptTimerImmediate:
    case TaskType::kDOMManipulation:
    case TaskType::kHistoryTraversal:
    case TaskType::kEmbed:
    case TaskType::kCanvasBlobSerialization:
    case TaskType::kRemoteEvent:
    case TaskType::kWebSocket:
    case TaskType::kMicrotask:
    case TaskType::kUnshippedPortMessage:
    case TaskType::kPresentation:
    case TaskType::kSensor:
    case TaskType::kPerformanceTimeline:
    case TaskType::kWebGL:
    case TaskType::kWebGPU:
    case TaskType::kInternalDefault:
    case TaskType::kMiscPlatformAPI:
    case TaskType::kFontLoading:
    case TaskType::kApplicationLifeCycle:
    case TaskType::kBackgroundFetch:
    case TaskType::kPermission:
    case TaskType::kStorage:
    case TaskType::kClipboard:
    case TaskType::kMachineLearning:
    case TaskType::kFileReading:
      return FrameQueue::kDeferrable;
    case TaskType::kPostedMessage:
    case TaskType::kUserInteraction:
    case TaskType::kMediaElementEvent:
    case TaskType::kInternalWebCrypto:
    case TaskType::kDatabaseAccess:
      return FrameQueue::kPausable;
    case TaskType::kInternalContinueScriptLoading:
      return FrameQueue::kInternalScriptContinuation;
    case TaskType::kInternalNavigationAssociated:
      return FrameQueue::kFreezable;
    case TaskType::kInternalTest:
    case TaskType::kWebLocks:
      return FrameQueue::kUnpausable;
    case TaskType::kInternalInspector:
      return FrameQueue::kInspector;
    case TaskType::kMainThreadTaskQueueCompositor:
      // The main thread's, which TaskRunner() gives itself.
      NOTREACHED();
  }
}

// FrameSchedulerImpl::ComputePriority() for each queue of a page that is
// visible.
TaskPriority PriorityOf(PageScheduler::FrameQueue queue) {
  using FrameQueue = PageScheduler::FrameQueue;
  switch (queue) {
    case FrameQueue::kInternalScriptContinuation:
      return TaskPriority::kVeryHighPriority;
    case FrameQueue::kRenderBlockingLoading:
      return TaskPriority::kExtremelyHighPriority;
    case FrameQueue::kLoadingControl:
      return TaskPriority::kHighPriority;
    case FrameQueue::kLowPriorityScriptExecution:
      return TaskPriority::kLowPriority;
    case FrameQueue::kLoading:
    case FrameQueue::kThrottleable:
    case FrameQueue::kDeferrable:
    case FrameQueue::kPausable:
    case FrameQueue::kUnpausable:
    case FrameQueue::kFreezable:
    case FrameQueue::kInspector:
      return TaskPriority::kNormalPriority;
  }
}

// MainThreadTaskQueue::NameForQueueType() for the queue type of each.
QueueName NameOf(PageScheduler::FrameQueue queue) {
  using FrameQueue = PageScheduler::FrameQueue;
  switch (queue) {
    case FrameQueue::kLoading:
    case FrameQueue::kRenderBlockingLoading:
    case FrameQueue::kLowPriorityScriptExecution:
      return QueueName::FRAME_LOADING_TQ;
    case FrameQueue::kLoadingControl:
      return QueueName::FRAME_LOADING_CONTROL_TQ;
    case FrameQueue::kThrottleable:
      return QueueName::FRAME_THROTTLEABLE_TQ;
    case FrameQueue::kDeferrable:
      return QueueName::FRAME_DEFERRABLE_TQ;
    case FrameQueue::kPausable:
    case FrameQueue::kInternalScriptContinuation:
    case FrameQueue::kFreezable:
      return QueueName::FRAME_PAUSABLE_TQ;
    case FrameQueue::kUnpausable:
    case FrameQueue::kInspector:
      return QueueName::FRAME_UNPAUSABLE_TQ;
  }
}

// FrameSchedulerImpl::ComputePriority() for scheduler.postTask()'s queues.
TaskPriority PriorityOf(EventLoop::Queue queue) {
  switch (queue) {
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
  const auto make_queue = [this](QueueName name, TaskPriority priority) {
    TaskQueue::Handle queue =
        sequence_manager_->CreateTaskQueue(TaskQueue::Spec(name));
    queue->SetQueuePriority(priority);
    return queue;
  };
  // MainThreadSchedulerImpl::ComputePriority(): the default queue, V8's and
  // the compositor's run at the normal priority, the last while rendering is
  // not starved.
  default_queue_ =
      make_queue(QueueName::DEFAULT_TQ, TaskPriority::kNormalPriority);
  v8_queue_ = make_queue(QueueName::V8_TQ, TaskPriority::kNormalPriority);
  compositor_queue_ =
      make_queue(QueueName::COMPOSITOR_TQ, TaskPriority::kNormalPriority);
  for (size_t i = 0; i < kFrameQueueCount; ++i) {
    const FrameQueue queue = static_cast<FrameQueue>(i);
    frame_queues_[i] = make_queue(NameOf(queue), PriorityOf(queue));
  }
  for (size_t i = 0; i < kWebSchedulingQueueCount; ++i) {
    web_scheduling_queues_[i] = make_queue(QueueName::WEB_SCHEDULING_TQ,
                                           PriorityOf(static_cast<Queue>(i)));
  }
  control_queue_ =
      make_queue(QueueName::CONTROL_TQ, TaskPriority::kControlPriority);
  idle_queue_ =
      make_queue(QueueName::IDLE_TQ, TaskPriority::kBestEffortPriority);
  sequence_manager_->SetDefaultTaskQueue(default_queue_.get());
  sequence_manager_->BindToMessagePump(
      base::MessagePump::Create(base::MessagePumpType::DEFAULT));
  idle_helper_ = std::make_unique<IdleHelper>(sequence_manager_.get(),
                                              control_queue_->task_runner(),
                                              idle_queue_.get());
  // MainThreadSchedulerImpl::BeginFrameNotExpectedSoon(): a page that draws
  // no frames has long idle periods from the start.
  idle_helper_->EnableLongIdlePeriod();
  CHECK(!EventLoop::Get());
  EventLoop::Set(this);
}

PageScheduler::~PageScheduler() {
  EventLoop::Set(nullptr);
  idle_helper_.reset();
}

const scoped_refptr<base::SingleThreadTaskRunner>& PageScheduler::task_runner()
    const {
  return default_queue_->task_runner();
}

scoped_refptr<base::SingleThreadTaskRunner> PageScheduler::V8TaskRunner()
    const {
  return v8_queue_->task_runner();
}

base::sequence_manager::TaskQueue* PageScheduler::CompositorTaskQueue() const {
  return compositor_queue_.get();
}

scoped_refptr<base::SingleThreadTaskRunner> PageScheduler::TaskRunner(
    TaskType type) {
  if (type == TaskType::kMainThreadTaskQueueCompositor) {
    return compositor_queue_->task_runner();
  }
  return frame_queues_[static_cast<size_t>(QueueOf(type))]->task_runner();
}

scoped_refptr<base::SingleThreadTaskRunner> PageScheduler::TaskRunner(
    Queue queue) {
  return web_scheduling_queues_[static_cast<size_t>(queue)]->task_runner();
}

void PageScheduler::PostIdleTask(const base::Location& from_here,
                                 IdleTask task) {
  idle_helper_->PostIdleTask(from_here, std::move(task));
}

}  // namespace mitto
