// Runs a Page's scripts on V8, in one context, on the Page program's main
// thread, and sends what each returns as JSON, with what they write to the
// console and the errors that they leave uncaught.

#ifndef MITTO_PAGE_SCRIPT_RUNNER_H_
#define MITTO_PAGE_SCRIPT_RUNNER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/task/task_observer.h"
#include "base/time/time.h"
#include "mitto/page/chrome_object.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-value.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

class DOMTimerCoordinator;
class Document;
class DocumentLoader;
class PageFetcher;
class PageRequests;
class SessionCookieJar;

// Fills `buffer` from the system's random source, which needs no file, for
// V8's random numbers, as Chrome's renderers do.
bool GenerateEntropy(unsigned char* buffer, size_t size);

// Whether a document at `url` is a secure context: the HTML Standard's
// potentially trustworthy URL, as Chrome decides it with no allowlist.
// Upstream: net/base/is_potentially_trustworthy.cc
bool IsUrlPotentiallyTrustworthy(const GURL& url);

// The main thread is the Page's event loop: each Session frame, each step of a
// document's loading, each timer, each part of a response and each of V8's own
// tasks is a task of it, and after each task this does what Blink's scheduler
// does: a microtask checkpoint, and then the report of the promises that it
// left rejected with no handler, from a task of its own.
class ScriptRunner final : public base::TaskObserver {
 public:
  explicit ScriptRunner(v8::Isolate* isolate);

  ScriptRunner(const ScriptRunner&) = delete;
  ScriptRunner& operator=(const ScriptRunner&) = delete;

  ~ScriptRunner() override;

  // Sets the time zone and the locale that ICU, and so V8, report, so that
  // neither is read from the machine, and makes the context, about:blank's,
  // whose window reads the rest of `start`, as each later window does.
  // Returns why not, if ICU does not know one of them. What the Page sends from
  // then on goes through `send`, and what the Page refuses of what the Session
  // sends through `refuse`. Documents that the Session loads read and write
  // their cookies through `cookie_jar`, which outlives this.
  std::optional<std::string> Start(
      const PageStart& start,
      SendFrame send,
      base::RepeatingCallback<void(std::string why)> refuse,
      SessionCookieJar& cookie_jar);

  // Runs `script` in the current context, and sends its result as JSON, or why
  // it has none. When the result is a promise, that waits until the promise
  // settles, which another script may do, and later scripts run meanwhile.
  void Evaluate(uint64_t call, std::string_view script);

  // What the Session sends of the Page's requests, each as a task.
  void OnResponseStarted(PageResponseStarted response);
  void OnResponseBody(PageResponseBody body);
  void OnResponseEnded(PageResponseEnded ended);

  // What a document's loading uses, on the main thread.
  //
  // Runs `task` in the current context. What Blink does after each task
  // follows once the task that runs this ends.
  void RunTask(base::OnceClosure task);
  // Runs `task` as RunTask() does, from a later task.
  void PostTask(base::OnceClosure task);
  // Makes a new context, whose window has an empty document at `url`, in
  // place of the current one, and returns the document. Calls that wait for a
  // promise of the old context fail, as DevTools' do when a page navigates,
  // and the old window's timers and fetches stop. The new window's
  // performance.timeOrigin is `navigation_start`, and its
  // navigator.connection gives `connection`.
  Document& CommitDocument(const GURL& url,
                           base::TimeTicks navigation_start,
                           const PageConnection& connection);
  // What window.chrome of the current window reads, which its document's
  // loading fills in.
  ChromeObjectData& chrome_object_data() { return *chrome_object_data_.back(); }
  // Runs a classic script from `url` in the current context, inside a task,
  // reports what it throws as an uncaught error, and then runs the microtasks
  // it queued, as the HTML Standard cleans up after running a script.
  void RunClassicScript(std::u16string_view source, const GURL& url);

  // base::TaskObserver:
  void WillProcessTask(const base::PendingTask& pending_task,
                       bool was_blocked_or_low_priority) override;
  void DidProcessTask(const base::PendingTask& pending_task) override;

 private:
  // What Blink does after each task: a microtask checkpoint, then a task that
  // reports the promises rejected with no handler, and the results that the
  // tasks settled once those reports have gone, so that the Session hears of a
  // task's uncaught errors before its results.
  void EndTask();
  // Reports those of `rejections` that still have no handler.
  void ReportRejections(std::vector<PageReporter::Rejection> rejections);
  // Makes a window's context, whose time origin is `time_origin`, and what
  // Chrome's renderer adds to it.
  v8::Local<v8::Context> NewContext(base::TimeTicks time_origin);
  // Gives the current context's window its timers, what its fetch() goes
  // through, and what it reads of the browser and the machine, with
  // `connection` for its document.
  void AttachWindow(v8::Local<v8::Context> context,
                    const PageConnection& connection);

  // Returns `value`, the result of `call`, as JSON, or why it cannot be.
  PageEvaluated ResultOf(uint64_t call,
                         v8::Local<v8::Context> context,
                         v8::Local<v8::Value> value);

  // Returns that `call` failed, and why.
  static PageEvaluated FailureOf(uint64_t call, std::string text);

  // Returns the result of `call` if `promise` has settled. If not, returns
  // nullopt and queues the result in `settled_` once it settles.
  std::optional<PageEvaluated> SettledResultOf(uint64_t call,
                                               v8::Local<v8::Context> context,
                                               v8::Local<v8::Promise> promise);

  // Returns the runner, and stores the call, that SettledResultOf() gave a
  // callback of the promise.
  static ScriptRunner& FromCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info,
      uint64_t& call);
  static void OnFulfilled(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnRejected(const v8::FunctionCallbackInfo<v8::Value>& info);

  const raw_ptr<v8::Isolate> isolate_;
  SendFrame send_;
  std::unique_ptr<PageReporter> reporter_;
  // What the Page started with, which each window reads.
  PageStart start_;
  // The results of the calls that the current task has settled, which go to
  // the Session once the task's uncaught errors have.
  std::vector<PageEvaluated> settled_;
  // The calls that wait for a promise to settle.
  std::set<uint64_t> waiting_calls_;
  raw_ptr<SessionCookieJar> cookie_jar_ = nullptr;
  std::unique_ptr<PageRequests> requests_;
  std::unique_ptr<DocumentLoader> loader_;
  // The current window's timers and what its fetch() goes through.
  std::unique_ptr<DOMTimerCoordinator> timers_;
  std::unique_ptr<PageFetcher> fetcher_;
  // What window.chrome of each window that the Page has made reads, the
  // current one's last. They live as long as the Page, for a callback of a
  // window that has gone.
  std::vector<std::unique_ptr<ChromeObjectData>> chrome_object_data_;
  // How many tasks that report rejections wait to run.
  int reports_queued_ = 0;
  v8::Global<v8::Context> context_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SCRIPT_RUNNER_H_
