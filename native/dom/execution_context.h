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
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class CacheStorageHost;
class IndexedDbHost;
class Modulator;
class StorageBucketHost;
class DOMTimerCoordinator;
class Performance;
class Request;
class Resource;
class EventTarget;
class Fetcher;
class WindowTasks;
class WorkerStarter;

class ExecutionContext : public cppgc::GarbageCollectedMixin {
 public:
  // The execution context of the isolate's current context.
  static ExecutionContext& Current();
  // The execution context of `context`, whose global object it is, or null
  // for a context of none: the one bound to it, or else its global proxy's.
  static ExecutionContext* From(v8::Isolate* isolate,
                                v8::Local<v8::Context> context);
  // Binds this to `context`, its global object's, so that From() finds it
  // there even once a frame's next window has taken the global proxy over,
  // as each of Blink's ScriptStates knows its own ExecutionContext.
  void BindToContext(v8::Isolate* isolate, v8::Local<v8::Context> context);

  ExecutionContext(const ExecutionContext&) = delete;
  ExecutionContext& operator=(const ExecutionContext&) = delete;

  virtual ~ExecutionContext();

  void Trace(cppgc::Visitor* visitor) const override {}

  // Whether it is a window, as Blink's IsWindow().
  virtual bool IsWindow() const;
  // The context's URL as a page reads it: the document's, or the worker's
  // script's final URL.
  virtual std::u16string Url() const = 0;
  // Where errors that the context's scripts leave uncaught fire, as Blink's
  // ErrorEventTarget(): the window, or the worker's global scope.
  virtual EventTarget* ErrorEventTarget() = 0;
  // The context's origin, as Blink's GetSecurityOrigin(): the document's, or
  // the worker's, its creator's.
  virtual url::Origin GetSecurityOrigin() const = 0;
  // `url` resolved against the context's base URL, as Blink's CompleteURL():
  // the document's, with its query in the document's encoding, or the
  // worker's URL, in UTF-8.
  virtual GURL CompleteUrl(std::u16string_view url) const = 0;
  // The base URL that an API resolves against in UTF-8, as KURL(base, input)
  // does: the document's, or the worker's URL.
  virtual GURL BaseUrl() const = 0;
  // WindowOrWorkerGlobalScope's origin, serialized.
  virtual std::u16string Origin() const = 0;
  // WindowOrWorkerGlobalScope's isSecureContext.
  virtual bool IsSecureContextForBindings() const = 0;
  // WindowOrWorkerGlobalScope's performance, made the first time.
  virtual mitto::Performance* Performance() = 0;
  // Where the context's Cache API reaches the Session, or null for a context
  // whose origin may not reach caches.
  virtual CacheStorageHost* GetCacheStorageHost() const = 0;
  // Where the context's IndexedDB reaches the Session, or null for a context
  // whose origin may not reach databases.
  virtual IndexedDbHost* GetIndexedDbHost() const = 0;
  // Where the context's Storage Buckets API reaches the Session, or null for
  // a context whose origin may not reach buckets.
  virtual StorageBucketHost* GetStorageBucketHost() const = 0;
  // The context's module map and the rest of its module machinery, made the
  // first time, as Blink's Modulator::From() makes the context's.
  virtual Modulator* GetModulator() = 0;

  // ExecutionContext's state of its error events (error_reporting.h).
  // An error that the error listeners threw, with the text of V8's message
  // of it.
  struct PendingException {
    PendingException(v8::Global<v8::Value> exception, std::u16string text);
    PendingException(PendingException&&);
    PendingException& operator=(PendingException&&);
    ~PendingException();

    v8::Global<v8::Value> exception;
    std::u16string text;
  };
  struct ErrorEventState {
    ErrorEventState();
    ~ErrorEventState();

    // Whether the error listeners run, which queues an error that they throw
    // for the console alone.
    bool in_dispatch_error_event = false;
    // Whether those queued errors go to the console, which drops any error
    // thrown meanwhile.
    bool draining_pending_exceptions = false;
    std::vector<PendingException> pending_exceptions;
  };
  ErrorEventState& GetErrorEventState() { return error_event_state_; }

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
  v8::Local<v8::Promise> Fetch(const bindings::RequestOrUSVString& input,
                               const bindings::RequestInit& init,
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
  // among no timers. What it returns lets RemoveContextDestroyedCallback()
  // drop it, for an observer that goes first.
  uint64_t AddContextDestroyedCallback(base::OnceClosure callback);
  void RemoveContextDestroyedCallback(uint64_t id);

  // What starts the context's dedicated workers. Without one, as in the Page
  // program given a script, a worker's script never comes.
  void SetWorkerStarter(WorkerStarter* starter) { worker_starter_ = starter; }
  WorkerStarter* GetWorkerStarter() const { return worker_starter_; }

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

  // What the context's DOMHighResTimeStamps count from: the start of the
  // navigation that loaded a window's document, or when a context was made
  // for about:blank or a worker. Each context of a Page has its own, as Blink
  // keeps one for each window's and worker's performance timeline.
  void SetTimeOrigin(base::TimeTicks time_origin) {
    time_origin_ = time_origin;
  }
  base::TimeTicks TimeOrigin() const { return time_origin_; }

  // Whether the context is a secure context, which its document's origin
  // decides, or a worker's creator, as Blink's
  // ExecutionContext::IsSecureContext() says. The Page sets it on the context
  // too, before the context installs what it decides
  // (bindings::SetSecureContext()).
  void SetSecureContext(bool secure) { secure_context_ = secure; }
  bool IsSecureContext() const { return secure_context_; }

  // The ID by which the Page names the context to its Session, which each
  // message of the context carries (page_messages.h). kNoDocument for a
  // context that no Session loaded, as in the Page program given a script.
  void SetContextId(uint64_t id) { context_id_ = id; }
  uint64_t ContextId() const { return context_id_; }

  // Blink's CrossOriginIsolatedCapability(): whether the context is
  // cross-origin isolated, which WindowOrWorkerGlobalScope's
  // crossOriginIsolated gives. A window's is its agent's, which the Session
  // decides for each document as Chrome's browser decides its agent cluster;
  // a dedicated worker's its creator's, but for a worker of a data: URL,
  // which never is. The Page sets it once its global object has a context,
  // before what the context decides is installed.
  bool CrossOriginIsolatedCapability() const {
    return cross_origin_isolated_capability_;
  }
  void SetCrossOriginIsolatedCapability(bool capability) {
    cross_origin_isolated_capability_ = capability;
  }
  // ExecutionContext::SharedArrayBufferTransferAllowed(): whether the context
  // has SharedArrayBuffer and may post one. Chrome allows it beside
  // cross-origin isolation only through the reverse origin trial and the
  // enterprise policy, which a Page has neither of.
  bool SharedArrayBufferTransferAllowed() const {
    return CrossOriginIsolatedCapability();
  }

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
  // The resource that a link preloaded as fetch which `request` takes, if
  // any, as Blink's MatchPreload() gives it.
  Resource* MatchFetchPreload(const Request& request);

  // Runs a callback that queueMicrotask() queued, from its microtask.
  static void RunQueuedMicrotask(void* data);

  ErrorEventState error_event_state_;
  ConsoleSink console_sink_;
  PageStart start_;
  PageConnection connection_;
  uint64_t context_id_ = kNoDocument;
  base::TimeTicks time_origin_ = base::TimeTicks::Now();
  // A context that nothing says otherwise of is one, as the Page program's
  // command line makes, as a file: page is.
  bool secure_context_ = true;
  bool cross_origin_isolated_capability_ = false;
  raw_ptr<DOMTimerCoordinator> timers_ = nullptr;
  raw_ptr<Fetcher> fetcher_ = nullptr;
  raw_ptr<WindowTasks> tasks_ = nullptr;
  raw_ptr<WorkerStarter> worker_starter_ = nullptr;
  std::map<uint64_t, base::OnceClosure> signal_timers_;
  // In the order they were added.
  std::map<uint64_t, base::OnceClosure> context_destroyed_callbacks_;
  uint64_t next_signal_timer_ = 0;
  uint64_t next_context_destroyed_callback_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_EXECUTION_CONTEXT_H_
