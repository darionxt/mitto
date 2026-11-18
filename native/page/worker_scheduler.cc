#include "mitto/page/worker_scheduler.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/notreached.h"
#include "base/task/sequence_manager/sequence_manager_impl.h"
#include "base/time/time.h"

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

// How long an idle task waits at most for its period: the long idle period of
// a thread that expects no frames.
constexpr base::TimeDelta kMaximumIdlePeriod = base::Milliseconds(50);

// WorkerSchedulerImpl::GetTaskRunner(): the queue of each task type that a
// worker posts.
WorkerScheduler::WorkerQueue QueueOf(TaskType type) {
  using WorkerQueue = WorkerScheduler::WorkerQueue;
  switch (type) {
    case TaskType::kJavascriptTimerImmediate:
    case TaskType::kJavascriptTimerDelayedLowNesting:
    case TaskType::kJavascriptTimerDelayedHighNesting:
    case TaskType::kPostedMessage:
      return WorkerQueue::kThrottleable;
    case TaskType::kNetworking:
    case TaskType::kNetworkingControl:
    case TaskType::kNetworkingUnfreezable:
    case TaskType::kNetworkingUnfreezableRenderBlockingLoading:
    case TaskType::kWebSocket:
    case TaskType::kInternalLoading:
    case TaskType::kFileReading:
      return WorkerQueue::kPausableNonVirtualTime;
    case TaskType::kInternalInspector:
    case TaskType::kInternalTest:
    case TaskType::kWebLocks:
      return WorkerQueue::kUnpausable;
    case TaskType::kDOMManipulation:
    case TaskType::kUserInteraction:
    case TaskType::kLowPriorityScriptExecution:
    case TaskType::kHistoryTraversal:
    case TaskType::kEmbed:
    case TaskType::kMediaElementEvent:
    case TaskType::kCanvasBlobSerialization:
    case TaskType::kMicrotask:
    case TaskType::kRemoteEvent:
    case TaskType::kUnshippedPortMessage:
    case TaskType::kDatabaseAccess:
    case TaskType::kPresentation:
    case TaskType::kSensor:
    case TaskType::kPerformanceTimeline:
    case TaskType::kWebGL:
    case TaskType::kWebGPU:
    case TaskType::kIdleTask:
    case TaskType::kMiscPlatformAPI:
    case TaskType::kFontLoading:
    case TaskType::kApplicationLifeCycle:
    case TaskType::kBackgroundFetch:
    case TaskType::kPermission:
    case TaskType::kStorage:
    case TaskType::kClipboard:
    case TaskType::kMachineLearning:
    case TaskType::kInternalDefault:
    case TaskType::kInternalWebCrypto:
    case TaskType::kInternalContinueScriptLoading:
    case TaskType::kInternalNavigationAssociated:
      return WorkerQueue::kPausable;
    case TaskType::kMainThreadTaskQueueCompositor:
      // A main thread's alone.
      NOTREACHED();
  }
}

// NonMainThreadTaskQueue::OnWebSchedulingPriorityChanged(): scheduler.
// postTask()'s priorities on a worker.
TaskPriority PriorityOf(EventLoop::Queue queue) {
  switch (queue) {
    case EventLoop::Queue::kUserBlocking:
      return TaskPriority::kHighPriority;
    case EventLoop::Queue::kUserBlockingContinuation:
      return TaskPriority::kHighPriorityContinuation;
    case EventLoop::Queue::kUserVisible:
      return TaskPriority::kNormalPriority;
    case EventLoop::Queue::kUserVisibleContinuation:
      return TaskPriority::kNormalPriorityContinuation;
    case EventLoop::Queue::kBackground:
      return TaskPriority::kLowPriority;
    case EventLoop::Queue::kBackgroundContinuation:
      return TaskPriority::kLowPriorityContinuation;
  }
}

}  // namespace

WorkerScheduler::WorkerScheduler() {
  SequenceManager::PrioritySettings priorities(TaskPriority::kPriorityCount,
                                               TaskPriority::kNormalPriority);
  sequence_manager_ = base::sequence_manager::CreateUnboundSequenceManager(
      SequenceManager::Settings::Builder()
          .SetMessagePumpType(base::MessagePumpType::DEFAULT)
          .SetPrioritySettings(std::move(priorities))
          .Build());
  const auto make_queue = [this](QueueName name, TaskPriority priority) {
    TaskQueue::Handle queue =
        sequence_manager_->CreateTaskQueue(TaskQueue::Spec(name));
    queue->SetQueuePriority(priority);
    return queue;
  };
  default_queue_ = make_queue(QueueName::SUBTHREAD_DEFAULT_TQ,
                              TaskPriority::kNormalPriority);
  v8_queue_ = make_queue(QueueName::V8_TQ, TaskPriority::kNormalPriority);
  // NonMainThreadSchedulerHelper's control queue.
  control_queue_ = make_queue(QueueName::SUBTHREAD_CONTROL_TQ,
                              TaskPriority::kControlPriority);
  idle_queue_ =
      make_queue(QueueName::WORKER_IDLE_TQ, TaskPriority::kBestEffortPriority);
  // WorkerThreadScheduler's compositor queue, of no priority of its own.
  compositor_queue_ =
      make_queue(QueueName::COMPOSITOR_TQ, TaskPriority::kNormalPriority);
  // WorkerSchedulerImpl's queues have no priority of their own.
  constexpr std::array<QueueName, kWorkerQueueCount> kNames = {
      QueueName::WORKER_THROTTLEABLE_TQ, QueueName::WORKER_PAUSABLE_TQ,
      QueueName::WORKER_PAUSABLE_TQ, QueueName::WORKER_UNPAUSABLE_TQ};
  for (size_t i = 0; i < kWorkerQueueCount; ++i) {
    worker_queues_[i] = make_queue(kNames[i], TaskPriority::kNormalPriority);
    worker_runners_[i] = worker_queues_[i]->task_runner();
  }
  for (size_t i = 0; i < kWebSchedulingQueueCount; ++i) {
    web_scheduling_queues_[i] = make_queue(QueueName::WORKER_WEB_SCHEDULING_TQ,
                                           PriorityOf(static_cast<Queue>(i)));
    web_scheduling_runners_[i] = web_scheduling_queues_[i]->task_runner();
  }
  default_runner_ = default_queue_->task_runner();
  v8_runner_ = v8_queue_->task_runner();
  control_runner_ = control_queue_->task_runner();
  idle_runner_ = idle_queue_->task_runner();
  sequence_manager_->SetDefaultTaskQueue(default_queue_.get());
}

WorkerScheduler::~WorkerScheduler() {
  if (EventLoop::Get() == this) {
    EventLoop::Set(nullptr);
  }
}

void WorkerScheduler::BindToCurrentThread() {
  sequence_manager_->BindToMessagePump(
      base::MessagePump::Create(base::MessagePumpType::DEFAULT));
  CHECK(!EventLoop::Get());
  EventLoop::Set(this);
}

void WorkerScheduler::Shutdown() {
  begin_frames_.reset();
  default_queue_.reset();
  v8_queue_.reset();
  control_queue_.reset();
  idle_queue_.reset();
  compositor_queue_.reset();
  for (TaskQueue::Handle& queue : worker_queues_) {
    queue.reset();
  }
  for (TaskQueue::Handle& queue : web_scheduling_queues_) {
    queue.reset();
  }
}

scoped_refptr<base::SingleThreadTaskRunner> WorkerScheduler::DefaultTaskRunner()
    const {
  return default_runner_;
}

scoped_refptr<base::SingleThreadTaskRunner> WorkerScheduler::V8TaskRunner()
    const {
  return v8_runner_;
}

scoped_refptr<base::SingleThreadTaskRunner> WorkerScheduler::ControlTaskRunner()
    const {
  return control_runner_;
}

scoped_refptr<base::SingleThreadTaskRunner> WorkerScheduler::TaskRunner(
    TaskType type) {
  return worker_runners_[static_cast<size_t>(QueueOf(type))];
}

scoped_refptr<base::SingleThreadTaskRunner> WorkerScheduler::TaskRunner(
    Queue queue) {
  return web_scheduling_runners_[static_cast<size_t>(queue)];
}

void WorkerScheduler::PostIdleTask(const base::Location& from_here,
                                   IdleTask task) {
  // A worker has no frames, so its idle periods are long ones, which last
  // until the next task, at most 50 milliseconds: it runs when nothing else
  // is ready.
  idle_runner_->PostTask(
      from_here,
      base::BindOnce(
          [](IdleTask task) {
            std::move(task).Run(base::TimeTicks::Now() + kMaximumIdlePeriod);
          },
          std::move(task)));
}

void WorkerScheduler::RequestBeginFrame(BeginFrameCallback begin_frame) {
  if (!compositor_queue_) {
    return;
  }
  if (!begin_frames_) {
    begin_frames_ = std::make_unique<WorkerBeginFrameProvider>(
        compositor_queue_->task_runner());
  }
  begin_frames_->RequestBeginFrame(std::move(begin_frame));
}

// A worker's frames follow the display's vsyncs, which the Page's set.
void WorkerScheduler::SetRefreshRate(int hertz) {}

}  // namespace mitto
