// The Page program's v8::Platform, as gin's V8Platform is Chrome's renderers':
// V8's tasks for the main thread are tasks of the Page's event loop, on the
// main thread's task runner, those for a dedicated worker's isolate tasks of
// its thread, and the clock that V8's Date reads is coarsened as Chrome
// coarsens it. Worker threads, jobs, the page allocator and tracing stay
// libplatform's.
//
// Upstream: gin/v8_platform.cc
// Upstream: gin/public/v8_platform.h
// Upstream: gin/v8_foreground_task_runner.cc

#ifndef MITTO_PAGE_PAGE_PLATFORM_H_
#define MITTO_PAGE_PAGE_PLATFORM_H_

#include <stdint.h>

#include <map>
#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "mitto/page/wall_clock_clamper.h"
#include "v8/include/v8-platform.h"

namespace mitto {

class PagePlatform final : public v8::Platform {
 public:
  // Posts V8's tasks for the main thread to `main_thread`, and hands the rest
  // to `delegate`, libplatform's default platform.
  PagePlatform(std::unique_ptr<v8::Platform> delegate,
               scoped_refptr<base::SingleThreadTaskRunner> main_thread);

  PagePlatform(const PagePlatform&) = delete;
  PagePlatform& operator=(const PagePlatform&) = delete;

  ~PagePlatform() override;

  // The process's, once the Page program has made it.
  static PagePlatform* Get();

  // Has V8's foreground tasks for `isolate`, a dedicated worker's, go to
  // `task_runner`, its thread's, until UnregisterIsolate(), as gin gives each
  // isolate a task runner of its own. Any thread.
  void RegisterIsolate(v8::Isolate* isolate,
                       scoped_refptr<base::SingleThreadTaskRunner> task_runner);
  void UnregisterIsolate(v8::Isolate* isolate);

  // v8::Platform:
  v8::PageAllocator* GetPageAllocator() override;
  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override;
  size_t GetZeroSegmentSize() override;
  void OnCriticalMemoryPressure() override;
  int NumberOfWorkerThreads() override;
  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate,
      v8::TaskPriority priority) override;
  bool IdleTasksEnabled(v8::Isolate* isolate) override;
  std::unique_ptr<v8::ScopedBlockingCall> CreateBlockingScope(
      v8::BlockingType blocking_type) override;
  double MonotonicallyIncreasingTime() override;
  int64_t CurrentClockTimeMilliseconds() override;
  double CurrentClockTimeMillis() override;
  double CurrentClockTimeMillisecondsHighResolution() override;
  StackTracePrinter GetStackTracePrinter() override;
  v8::TracingController* GetTracingController() override;

 protected:
  // v8::Platform:
  std::unique_ptr<v8::JobHandle> CreateJobImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> job_task,
      const v8::SourceLocation& location) override;
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                  std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override;
  void PostDelayedTaskOnWorkerThreadImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override;

 private:
  const std::unique_ptr<v8::Platform> delegate_;
  // The main thread's isolate, and each worker's, has one runner for all of
  // its priorities, as they share one queue of the isolate's thread.
  const std::shared_ptr<v8::TaskRunner> main_thread_;
  base::Lock lock_;
  // V8 asks for a runner from its own threads too.
  std::map<v8::Isolate*, std::shared_ptr<v8::TaskRunner>> workers_
      GUARDED_BY(lock_);
  const WallClockClamper clamper_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_PLATFORM_H_
