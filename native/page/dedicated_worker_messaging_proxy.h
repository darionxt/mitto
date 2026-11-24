// The creator's side of a dedicated worker, as Blink's DedicatedWorker,
// DedicatedWorkerMessagingProxy and the browser's DedicatedWorkerHost run it
// for a Page: the worker's script comes through the Session as a request of
// Kind `worker` of the creating context, the worker's thread starts once its
// response has started, as PlzDedicatedWorker starts it, and from then on the
// proxy hands the worker its script and messages, and the Worker what the
// worker sends back, each from a task of the creator's.
//
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker.cc
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_messaging_proxy.cc
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_object_proxy.cc
// Upstream: third_party/blink/renderer/core/workers/threaded_messaging_proxy_base.cc
// Upstream: content/browser/worker_host/worker_script_fetcher.cc

#ifndef MITTO_PAGE_DEDICATED_WORKER_MESSAGING_PROXY_H_
#define MITTO_PAGE_DEDICATED_WORKER_MESSAGING_PROXY_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/page/dedicated_worker_thread.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class ExecutionContext;
class MessagePortChannel;

// Where an error that a worker left uncaught goes once neither its Worker nor
// its creator's global object canceled it: the Session, for a window, or the
// creator's own Worker, for a worker that a worker started.
using ReportUncaughtWorkerError =
    base::RepeatingCallback<void(const ErrorMessageInfo& error)>;

class DedicatedWorkerMessagingProxy final : public WorkerMessagingProxy,
                                            public PageRequests::Client {
 public:
  // What the proxy takes of its creating context, a window's or a worker's.
  struct Creator {
    explicit Creator(PageRequests& requests);
    Creator(const Creator&);
    Creator& operator=(const Creator&);
    ~Creator();

    raw_ref<PageRequests> requests;
    scoped_refptr<SessionSender> sender;
    base::RepeatingCallback<void(std::string why)> refuse;
    ReportUncaughtWorkerError report_uncaught;
  };

  // Starts a worker of the script at `url` for `worker`, in the current
  // context.
  DedicatedWorkerMessagingProxy(Worker& worker,
                                Creator creator,
                                const GURL& url,
                                const WorkerStartOptions& options);

  DedicatedWorkerMessagingProxy(const DedicatedWorkerMessagingProxy&) = delete;
  DedicatedWorkerMessagingProxy& operator=(
      const DedicatedWorkerMessagingProxy&) = delete;

  ~DedicatedWorkerMessagingProxy() override;

  // WorkerMessagingProxy:
  void PostMessageToWorkerGlobalScope(SerializedScriptValue message,
                                      std::vector<MessagePortChannel> ports,
                                      bool user_activation) override;
  void Terminate() override;

  // PageRequests::Client, for the worker's script:
  void OnResponseStarted(const PageResponseStarted& response) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseEnded(int net_error) override;

  // What the worker's thread tells it, each from a task of the creator's.
  void DispatchMessage(SerializedScriptValue message,
                       std::vector<MessagePortChannel> ports);
  void DispatchErrorEvent(ErrorMessageInfo error);
  // An entry of the creator's Performance Timeline for a resource of the
  // worker's top-level module graph, while the creator's context remains.
  void AddResourceTiming(ResourceTimingInfo info,
                         std::u16string initiator_type);
  void DidEvaluateScript();
  void DidFailToFetchScript();
  void TerminateFromWorker();
  void WorkerThreadTerminated();

 private:
  // A message that waits for the top-level script to have run.
  struct QueuedMessage {
    QueuedMessage(SerializedScriptValue message,
                  std::vector<MessagePortChannel> ports,
                  bool user_activation);
    QueuedMessage(QueuedMessage&&);
    QueuedMessage& operator=(QueuedMessage&&);
    ~QueuedMessage();

    SerializedScriptValue message;
    std::vector<MessagePortChannel> ports;
    bool user_activation = false;
  };

  // Tells the Session of the worker, which `started` describes, and starts
  // its thread.
  void StartThread(PageWorkerStarted started);
  // Runs `run` in the creator's context, with the Worker, if both remain.
  void RunWithWorker(base::OnceCallback<void(Worker&)> run);

  cppgc::WeakPersistent<Worker> worker_;
  // The creator's context, which the Worker's events run in.
  cppgc::WeakPersistent<ExecutionContext> context_;
  Creator creator_;
  WorkerStartOptions options_;
  // What the worker's thread starts with, until it starts.
  DedicatedWorkerThread::Params params_;
  // The request of the worker's script, until it ends.
  uint64_t script_request_ = 0;
  bool script_requested_ = false;
  DedicatedWorkerThread::ScriptResponse response_;
  bool response_started_ = false;
  std::unique_ptr<DedicatedWorkerThread> thread_;
  bool evaluated_ = false;
  bool terminated_ = false;
  std::deque<QueuedMessage> queued_messages_;
  base::WeakPtrFactory<DedicatedWorkerMessagingProxy> weak_factory_{this};
};

// What starts the dedicated workers of one context, a window's or a
// worker's, through its requests.
class PageWorkerStarter final : public WorkerStarter {
 public:
  explicit PageWorkerStarter(DedicatedWorkerMessagingProxy::Creator creator);

  PageWorkerStarter(const PageWorkerStarter&) = delete;
  PageWorkerStarter& operator=(const PageWorkerStarter&) = delete;

  ~PageWorkerStarter() override;

  // WorkerStarter:
  std::unique_ptr<WorkerMessagingProxy> StartWorker(
      Worker& worker,
      const GURL& url,
      const WorkerStartOptions& options) override;

  // Terminates the workers that it started, and those that wait for their
  // scripts never start.
  void TerminateWorkers();

 private:
  DedicatedWorkerMessagingProxy::Creator creator_;
  // The Workers that it started, while they remain.
  std::vector<cppgc::WeakPersistent<Worker>> workers_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_DEDICATED_WORKER_MESSAGING_PROXY_H_
