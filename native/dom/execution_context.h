// What a realm of a Page runs in, as Blink's ExecutionContext: the window of a
// document, and the global scope of a worker once a Page has workers. It holds
// what both have of WindowOrWorkerGlobalScope, the timers, queueMicrotask(),
// atob(), btoa() and reportError(), and what the Page program gives each
// context: its tasks, its timers, what its requests go through, where its
// console messages go, its ID, and what it reads of the browser and the
// machine.
//
// Upstream: third_party/blink/renderer/core/execution_context/execution_context.h
// Upstream: third_party/blink/renderer/core/frame/window_or_worker_global_scope.cc
// Upstream: third_party/blink/renderer/core/frame/universal_global_scope.cc

#ifndef MITTO_DOM_EXECUTION_CONTEXT_H_
#define MITTO_DOM_EXECUTION_CONTEXT_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class DOMTimerCoordinator;
class Fetcher;
class WindowTasks;

class ExecutionContext : public cppgc::GarbageCollectedMixin {
 public:
  // The execution context of the isolate's current context.
  static ExecutionContext& Current();
  // The execution context of `context`, whose global object it is, or null
  // for a context of none.
  static ExecutionContext* From(v8::Isolate* isolate,
                                v8::Local<v8::Context> context);

  ExecutionContext(const ExecutionContext&) = delete;
  ExecutionContext& operator=(const ExecutionContext&) = delete;

  virtual ~ExecutionContext();

  void Trace(cppgc::Visitor* visitor) const override {}

  // Whether it is a window, as Blink's IsWindow().
  virtual bool IsWindow() const;

  // WindowOrWorkerGlobalScope's timers. Without timers, as once the context
  // is gone, a timer set is never run, and its ID is 0, as Chrome gives in a
  // destroyed context.
  int32_t SetTimeout(v8::Local<v8::Function> handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetTimeout(const std::u16string& handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments,
                     bindings::ExceptionState& exception_state);
  void ClearTimeout(int32_t handle);
  int32_t SetInterval(v8::Local<v8::Function> handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetInterval(const std::u16string& handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments,
                      bindings::ExceptionState& exception_state);
  void ClearInterval(int32_t handle);
  void QueueMicrotask(v8::Local<v8::Function> callback);
  std::u16string Btoa(const std::u16string& data,
                      bindings::ExceptionState& exception_state);
  std::u16string Atob(const std::u16string& data,
                      bindings::ExceptionState& exception_state);
  // Reports `e` as an error that a script left uncaught: the error event of
  // the context's global object, then the console, as Blink's
  // WindowOrWorkerGlobalScope does.
  void ReportError(v8::Local<v8::Value> e);

  // The timers of the context, which the Page program keeps, and which go
  // with it. Setting none stops the timers of AbortSignal.timeout() too, and
  // runs what goes with the context.
  void SetTimers(DOMTimerCoordinator* timers);
  // How many timers are set, those of AbortSignal.timeout() among them.
  virtual size_t TimerCount() const;
  // Whether the Page still runs the context: it gives the context its timers
  // until the context goes.
  bool HasContext() const { return timers_ != nullptr; }
  // The timer of an AbortSignal.timeout(), which counts among the context's
  // timers until RemoveSignalTimer() takes it back with the ID that this
  // returns. Its `cancel` runs once the Page lets the context go, as Blink's
  // ContextDestroyed() cancels the signal's task.
  uint64_t AddSignalTimer(base::OnceClosure cancel);
  void RemoveSignalTimer(uint64_t id);
  // What else goes with the context, such as the locks of a LockManager:
  // `callback` runs once the Page lets the context go, as Blink's
  // ExecutionContextLifecycleObserver::ContextDestroyed() does. It counts
  // among no timers.
  void AddContextDestroyedCallback(base::OnceClosure callback);

  // What fetch() asks the Session through. Without one, it fails.
  void SetFetcher(Fetcher* fetcher) { fetcher_ = fetcher; }
  Fetcher* GetFetcher() const { return fetcher_; }
  // The tasks of the context, which the Page program keeps, and which go with
  // it: without them, a task posted to the context never runs.
  void SetTasks(WindowTasks* tasks) { tasks_ = tasks; }
  WindowTasks* GetTasks() const { return tasks_; }

  // What the context reads of the browser and the machine, from the Page's
  // start, and what navigator.connection gives it. Without them, as in the
  // Page program given a script, it reads what a default PageStart holds.
  void SetStart(const PageStart& start, const PageConnection& connection);
  const PageStart& Start() const { return start_; }
  const PageConnection& Connection() const { return connection_; }

  // The ID by which the Page names the context to its Session, which each
  // message of the context carries (page_messages.h). kNoDocument for a
  // context that no Session loaded, as in the Page program given a script.
  void SetContextId(uint64_t id) { context_id_ = id; }
  uint64_t ContextId() const { return context_id_; }

  // Where the console messages of the DOM go, such as a warning of Blink's.
  // Without one, nowhere.
  using ConsoleSink =
      base::RepeatingCallback<void(ConsoleLevel level, std::string text)>;
  void SetConsoleSink(ConsoleSink sink) { console_sink_ = std::move(sink); }
  // ExecutionContext::AddConsoleMessage().
  void AddConsoleMessage(ConsoleLevel level, std::string text);

 protected:
  ExecutionContext();

 private:
  // Runs a callback that queueMicrotask() queued, from its microtask.
  static void RunQueuedMicrotask(void* data);

  ConsoleSink console_sink_;
  PageStart start_;
  PageConnection connection_;
  uint64_t context_id_ = kNoDocument;
  raw_ptr<DOMTimerCoordinator> timers_ = nullptr;
  raw_ptr<Fetcher> fetcher_ = nullptr;
  raw_ptr<WindowTasks> tasks_ = nullptr;
  std::map<uint64_t, base::OnceClosure> signal_timers_;
  std::vector<base::OnceClosure> context_destroyed_callbacks_;
  uint64_t next_signal_timer_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_EXECUTION_CONTEXT_H_
