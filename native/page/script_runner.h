// Runs a Page's scripts on V8, on the Page program's main thread, and sends
// what each returns as JSON, with what they write to the console and the
// errors that they leave uncaught. It holds what is the isolate's, and the
// Page's frames hold their windows' contexts (page_frame.h).

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
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"
#include "url/origin.h"
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

class CacheStorageHost;
class IndexedDbHost;
class PageFrame;
class SessionHistory;
class PageRequests;
class SessionCacheStorage;
class SessionCookieJar;
class SessionIndexedDb;
class SessionStorageBuckets;
class StorageBucketHost;

// Fills `buffer` from the system's random source, which needs no file, for
// V8's random numbers, as Chrome's renderers do.
bool GenerateEntropy(unsigned char* buffer, size_t size);

// Whether a top-level document of `origin` is a secure context: the HTML
// Standard's potentially trustworthy origin, as Chrome decides it with no
// allowlist. An opaque origin is not one, such as that of a tab's first
// about:blank, which has no creator to take an origin from, or of a data: URL.
// Upstream: services/network/public/cpp/is_potentially_trustworthy.cc
bool IsOriginPotentiallyTrustworthy(const url::Origin& origin);

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
  // neither is read from the machine, and makes the top-level frame, whose
  // first window, about:blank's, reads the rest of `start`, as each later
  // window does. Returns why not, if ICU does not know one of them. What the
  // Page sends from then on goes through `sender`, and what the Page refuses of
  // what the Session sends through `refuse`. Documents that the Session loads
  // read and write their cookies through `cookie_jar`, which outlives this.
  std::optional<std::string> Start(
      const PageStart& start,
      scoped_refptr<SessionSender> sender,
      base::RepeatingCallback<void(std::string why)> refuse,
      SessionCookieJar& cookie_jar);

  // Runs `script` in the context of the top-level frame's window, and sends
  // its result as JSON, or why it has none. When the result is a promise, that
  // waits until the promise settles, which another script may do, and later
  // scripts run meanwhile.
  void Evaluate(uint64_t call, std::string_view script);

  // Where the documents that the Session loads over HTTP or HTTPS reach its
  // Cache API, which outlives this.
  void SetCacheStorageHost(CacheStorageHost& host) {
    cache_storage_host_ = &host;
  }
  // Where the documents that the Session loads over HTTP or HTTPS reach its
  // IndexedDB, which outlives this.
  void SetIndexedDbHost(IndexedDbHost& host) { indexed_db_host_ = &host; }
  void SetStorageBucketHost(StorageBucketHost& host) {
    storage_bucket_host_ = &host;
  }

  // What the Session sends of the Page's requests, each as a task, which
  // runs in the context that made the request.
  void OnResponseStarted(PageResponseStarted response);
  void OnResponseBody(PageResponseBody body);
  void OnResponseEnded(PageResponseEnded ended);
  void OnRequestBodyCredit(PageCredit credit);
  void OnIsolation(PageIsolation isolation);

  // Runs `task` with a handle scope, in no context: what it runs enters the
  // context it belongs to. What Blink does after each task follows once the
  // task that runs this ends.
  void RunTask(base::OnceClosure task);

  // What the Page's frames use, on the main thread.
  v8::Isolate* isolate() const { return isolate_; }
  const PageStart& start() const { return start_; }
  const scoped_refptr<SessionSender>& sender() const { return sender_; }
  const SendFrame& send() const { return send_; }
  const base::RepeatingCallback<void(std::string why)>& refuse() const {
    return refuse_;
  }
  PageReporter& reporter() { return *reporter_; }
  PageRequests& requests() { return *requests_; }
  SessionCookieJar& cookie_jar() { return *cookie_jar_; }
  // The tab's session history, which the top-level frame keeps.
  SessionHistory& session_history() { return *session_history_; }
  CacheStorageHost* cache_storage_host() const { return cache_storage_host_; }
  IndexedDbHost* indexed_db_host() const { return indexed_db_host_; }
  StorageBucketHost* storage_bucket_host() const {
    return storage_bucket_host_;
  }
  // What window.chrome of a new window reads, whose loading starts at
  // `navigation_start`, which lives as long as the Page, for a callback of a
  // window that has gone.
  ChromeObjectData& NewChromeObjectData(base::TimeTicks navigation_start);
  // The top-level frame's window loses its context: the calls that wait for
  // a promise of it fail, as DevTools' do when a page navigates.
  void TopLevelContextGoes();
  // The ID of the Page's next frame, larger than each before it.
  uint64_t NextFrameId() { return ++last_frame_id_; }
  // How many frames the Page has, its top-level one among them.
  size_t FrameCount() const;
  // Keeps a removed frame's Cache API, IndexedDB and Storage Buckets until the
  // Page ends: what the frame's scripts made of them may still call them,
  // though the Session hears nothing more of a document that has gone.
  void KeepRemovedFrameStorage(
      std::unique_ptr<SessionCacheStorage> cache_storage,
      std::unique_ptr<SessionIndexedDb> indexed_db,
      std::unique_ptr<SessionStorageBuckets> storage_buckets);
  // Keeps a removed frame until the task ends: a script of the frame, or of a
  // frame under it, may be what removed it, and still runs.
  void KeepRemovedFrame(std::unique_ptr<PageFrame> frame);

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
  // V8's callback at the end of every microtask checkpoint, as Blink's
  // EventLoop::RunEndOfMicrotaskCheckpointTasks() notifies about rejected
  // promises there: so the checkpoint after each callback of a frame, or of a
  // listener, queues its report before what the next callback queues.
  static void OnMicrotasksCompleted(v8::Isolate* isolate, void* data);
  // Queues a task that reports the promises rejected with no handler since
  // the last checkpoint, if there are any.
  void QueueRejectionReport();
  // Reports those of `rejections` that still have no handler.
  void ReportRejections(std::vector<PageReporter::Rejection> rejections);

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
  scoped_refptr<SessionSender> sender_;
  SendFrame send_;
  base::RepeatingCallback<void(std::string why)> refuse_;
  std::unique_ptr<PageReporter> reporter_;
  // What the Page started with, which each window reads.
  PageStart start_;
  // The results of the calls that the current task has settled, which go to
  // the Session once the task's uncaught errors have.
  std::vector<PageEvaluated> settled_;
  // The calls that wait for a promise to settle.
  std::set<uint64_t> waiting_calls_;
  raw_ptr<SessionCookieJar> cookie_jar_ = nullptr;
  raw_ptr<CacheStorageHost> cache_storage_host_ = nullptr;
  raw_ptr<IndexedDbHost> indexed_db_host_ = nullptr;
  raw_ptr<StorageBucketHost> storage_bucket_host_ = nullptr;
  std::unique_ptr<PageRequests> requests_;
  // The tab's session history, which outlives its documents.
  std::unique_ptr<SessionHistory> session_history_;
  // The Page's top-level frame, which loads its documents.
  std::unique_ptr<PageFrame> frame_;
  // The frames removed in the current task, as KeepRemovedFrame() keeps them.
  std::vector<std::unique_ptr<PageFrame>> removed_frames_;
  // What window.chrome of each window that the Page has made reads. They live
  // as long as the Page, for a callback of a window that has gone.
  std::vector<std::unique_ptr<ChromeObjectData>> chrome_object_data_;
  // Removed frames' storage, as KeepRemovedFrameStorage() keeps it.
  std::vector<std::unique_ptr<SessionCacheStorage>> removed_cache_storage_;
  std::vector<std::unique_ptr<SessionIndexedDb>> removed_indexed_db_;
  std::vector<std::unique_ptr<SessionStorageBuckets>> removed_storage_buckets_;
  // How many tasks that report rejections wait to run.
  int reports_queued_ = 0;
  // Whether Start() made the top-level frame's first window.
  bool started_ = false;
  // The ID of the Page's last frame: 1 is the top-level frame's.
  uint64_t last_frame_id_ = 1;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SCRIPT_RUNNER_H_
