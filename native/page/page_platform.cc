#include "mitto/page/page_platform.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/time/time.h"

namespace mitto {
namespace {

// V8's tasks for the main thread, as tasks of the main thread's task runner.
class MainThreadTaskRunner final : public v8::TaskRunner {
 public:
  explicit MainThreadTaskRunner(
      scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {}

  MainThreadTaskRunner(const MainThreadTaskRunner&) = delete;
  MainThreadTaskRunner& operator=(const MainThreadTaskRunner&) = delete;

  ~MainThreadTaskRunner() override = default;

  // v8::TaskRunner:
  bool IdleTasksEnabled() override { return false; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 protected:
  // v8::TaskRunner:
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&v8::Task::Run, std::move(task)));
  }
  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override {
    task_runner_->PostNonNestableTask(
        FROM_HERE, base::BindOnce(&v8::Task::Run, std::move(task)));
  }
  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay_in_seconds,
                           const v8::SourceLocation& location) override {
    task_runner_->PostDelayedTask(
        FROM_HERE, base::BindOnce(&v8::Task::Run, std::move(task)),
        base::Seconds(delay_in_seconds));
  }
  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    task_runner_->PostNonNestableDelayedTask(
        FROM_HERE, base::BindOnce(&v8::Task::Run, std::move(task)),
        base::Seconds(delay_in_seconds));
  }

 private:
  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
};

}  // namespace

PagePlatform::PagePlatform(
    std::unique_ptr<v8::Platform> delegate,
    scoped_refptr<base::SingleThreadTaskRunner> main_thread)
    : delegate_(std::move(delegate)),
      main_thread_(
          std::make_shared<MainThreadTaskRunner>(std::move(main_thread))) {}

PagePlatform::~PagePlatform() = default;

v8::PageAllocator* PagePlatform::GetPageAllocator() {
  return delegate_->GetPageAllocator();
}

v8::ThreadIsolatedAllocator* PagePlatform::GetThreadIsolatedAllocator() {
  return delegate_->GetThreadIsolatedAllocator();
}

size_t PagePlatform::GetZeroSegmentSize() {
  return delegate_->GetZeroSegmentSize();
}

void PagePlatform::OnCriticalMemoryPressure() {
  delegate_->OnCriticalMemoryPressure();
}

int PagePlatform::NumberOfWorkerThreads() {
  return delegate_->NumberOfWorkerThreads();
}

std::shared_ptr<v8::TaskRunner> PagePlatform::GetForegroundTaskRunner(
    v8::Isolate* isolate,
    v8::TaskPriority priority) {
  return main_thread_;
}

bool PagePlatform::IdleTasksEnabled(v8::Isolate* isolate) {
  return false;
}

std::unique_ptr<v8::ScopedBlockingCall> PagePlatform::CreateBlockingScope(
    v8::BlockingType blocking_type) {
  return delegate_->CreateBlockingScope(blocking_type);
}

double PagePlatform::MonotonicallyIncreasingTime() {
  return (base::TimeTicks::Now() - base::TimeTicks()).InSecondsF();
}

int64_t PagePlatform::CurrentClockTimeMilliseconds() {
  return clamper_.ClampToMillis(base::Time::Now());
}

double PagePlatform::CurrentClockTimeMillis() {
  return static_cast<double>(clamper_.ClampToMillis(base::Time::Now()));
}

double PagePlatform::CurrentClockTimeMillisecondsHighResolution() {
  return clamper_.ClampToMillisHighResolution(base::Time::Now());
}

v8::Platform::StackTracePrinter PagePlatform::GetStackTracePrinter() {
  return delegate_->GetStackTracePrinter();
}

v8::TracingController* PagePlatform::GetTracingController() {
  return delegate_->GetTracingController();
}

std::unique_ptr<v8::JobHandle> PagePlatform::CreateJobImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::JobTask> job_task,
    const v8::SourceLocation& location) {
  return delegate_->CreateJob(priority, std::move(job_task), location);
}

void PagePlatform::PostTaskOnWorkerThreadImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::Task> task,
    const v8::SourceLocation& location) {
  delegate_->PostTaskOnWorkerThread(priority, std::move(task), location);
}

void PagePlatform::PostDelayedTaskOnWorkerThreadImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds,
    const v8::SourceLocation& location) {
  delegate_->PostDelayedTaskOnWorkerThread(priority, std::move(task),
                                           delay_in_seconds, location);
}

}  // namespace mitto
