#include "mitto/page/dedicated_worker_messaging_proxy.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/local_scheme_fetch.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "mitto/dom/worker.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// Blink's kForcibleTerminationDelay: how long a worker's script may run once
// its Worker has terminated it.
constexpr base::TimeDelta kForcibleTerminationDelay = base::Seconds(2);

// The worker's side of the proxy, DedicatedWorkerObjectProxy: what the worker
// tells its Worker, each posted to the creator's thread on the queue of its
// task type.
class ObjectProxy final : public DedicatedWorkerObjectProxy {
 public:
  explicit ObjectProxy(base::WeakPtr<DedicatedWorkerMessagingProxy> proxy)
      : proxy_(std::move(proxy)),
        posted_message_(EventLoop::Get()->TaskRunner(TaskType::kPostedMessage)),
        internal_default_(
            EventLoop::Get()->TaskRunner(TaskType::kInternalDefault)),
        internal_loading_(
            EventLoop::Get()->TaskRunner(TaskType::kInternalLoading)) {}

  ObjectProxy(const ObjectProxy&) = delete;
  ObjectProxy& operator=(const ObjectProxy&) = delete;

  ~ObjectProxy() override = default;

  // WorkerObjectProxy:
  void PostMessageToWorkerObject(
      SerializedScriptValue message,
      std::vector<MessagePortChannel> ports) override {
    posted_message_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::DispatchMessage, proxy_,
                       std::move(message), std::move(ports)));
  }
  void ReportException(std::u16string message,
                       std::u16string filename,
                       uint32_t line,
                       uint32_t column) override {
    ErrorMessageInfo error;
    error.message = std::move(message);
    error.filename = std::move(filename);
    error.line = line;
    error.column = column;
    internal_default_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::DispatchErrorEvent,
                       proxy_, std::move(error)));
  }
  void AddOutsideResourceTiming(ResourceTimingInfo info,
                                std::u16string initiator_type) override {
    internal_loading_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::AddResourceTiming,
                       proxy_, std::move(info), std::move(initiator_type)));
  }

  // DedicatedWorkerObjectProxy:
  void DidEvaluateTopLevelScript() override {
    internal_loading_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::DidEvaluateScript,
                       proxy_));
  }
  void DidFailToFetchScript() override {
    internal_loading_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::DidFailToFetchScript,
                       proxy_));
  }
  void DidCloseWorkerGlobalScope() override {
    internal_default_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::TerminateFromWorker,
                       proxy_));
  }
  void DidTerminateWorkerThread() override {
    internal_default_->PostTask(
        FROM_HERE,
        base::BindOnce(&DedicatedWorkerMessagingProxy::WorkerThreadTerminated,
                       proxy_));
  }

 private:
  const base::WeakPtr<DedicatedWorkerMessagingProxy> proxy_;
  const scoped_refptr<base::SingleThreadTaskRunner> posted_message_;
  const scoped_refptr<base::SingleThreadTaskRunner> internal_default_;
  const scoped_refptr<base::SingleThreadTaskRunner> internal_loading_;
};

}  // namespace

DedicatedWorkerMessagingProxy::Creator::Creator(PageRequests& requests)
    : requests(requests) {}
DedicatedWorkerMessagingProxy::Creator::Creator(const Creator&) = default;
DedicatedWorkerMessagingProxy::Creator&
DedicatedWorkerMessagingProxy::Creator::operator=(const Creator&) = default;
DedicatedWorkerMessagingProxy::Creator::~Creator() = default;

DedicatedWorkerMessagingProxy::QueuedMessage::QueuedMessage(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> ports,
    bool user_activation)
    : message(std::move(message)),
      ports(std::move(ports)),
      user_activation(user_activation) {}
DedicatedWorkerMessagingProxy::QueuedMessage::QueuedMessage(QueuedMessage&&) =
    default;
DedicatedWorkerMessagingProxy::QueuedMessage&
DedicatedWorkerMessagingProxy::QueuedMessage::operator=(QueuedMessage&&) =
    default;
DedicatedWorkerMessagingProxy::QueuedMessage::~QueuedMessage() = default;

DedicatedWorkerMessagingProxy::DedicatedWorkerMessagingProxy(
    Worker& worker,
    Creator creator,
    const GURL& url,
    const WorkerStartOptions& options)
    : worker_(&worker),
      context_(&ExecutionContext::Current()),
      creator_(std::move(creator)),
      options_(options) {
  ExecutionContext& context = *context_;
  bindings::PerIsolateData* const data =
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent());
  // GlobalScopeCreationParams: what the worker takes of its creator.
  params_.origin = context.GetSecurityOrigin();
  params_.name = options.name;
  params_.script_url = url;
  params_.module = options.module;
  params_.credentials = options.credentials;
  if (context.IsWindow()) {
    params_.referrer_policy =
        static_cast<Window&>(context).GetDocument().GetReferrerPolicy();
  }
  params_.strict_mime_type = !context.IsWindow();
  params_.secure_context = data->secure_context();
  params_.begin_frames = context.IsWindow();
  params_.start = context.Start();
  params_.connection = context.Connection();
  params_.features = data->features();
  params_.sender = creator_.sender;
  params_.refuse = creator_.refuse;
  // A data: URL's script never reaches the network, and its worker has an
  // opaque origin.
  if (url.SchemeIs(url::kDataScheme)) {
    params_.origin = url::Origin();
    std::optional<DataUrlResponse> data_url = ParseDataUrl(url);
    if (!data_url) {
      EventLoop::Get()
          ->TaskRunner(TaskType::kInternalLoading)
          ->PostTask(FROM_HERE,
                     base::BindOnce(
                         &DedicatedWorkerMessagingProxy::DidFailToFetchScript,
                         weak_factory_.GetWeakPtr()));
      return;
    }
    response_started_ = true;
    response_.url = url;
    response_.status = 200;
    response_.headers = "Content-Type: " + data_url->content_type + "\n";
    response_.body = std::move(data_url->body);
    PageWorkerStarted started;
    started.creator = context.ContextId();
    started.source = PageWorkerSource::kData;
    started.module = options_.module;
    StartThread(std::move(started));
    // As the browser's loader hands it over, from a task of its own.
    EventLoop::Get()
        ->TaskRunner(TaskType::kInternalLoading)
        ->PostTask(
            FROM_HERE,
            base::BindOnce(&DedicatedWorkerMessagingProxy::OnResponseEnded,
                           weak_factory_.GetWeakPtr(), /*net_error=*/0));
    return;
  }
  // A blob: URL's script comes from the blob that it names now, as Blink
  // resolves the URL to a token when the Worker is made, which the browser's
  // BlobURLLoader then reads, so revoking the URL after does not stop it. A
  // blob:null URL's worker, of an opaque origin, is a Known Gap.
  if (url.SchemeIsBlob()) {
    std::optional<BlobUrlRegistry::Entry> entry =
        BlobUrlRegistry::Get().ResolveEntry(url);
    if (!entry || !entry->blob || url::Origin::Create(url).opaque()) {
      EventLoop::Get()
          ->TaskRunner(TaskType::kInternalLoading)
          ->PostTask(FROM_HERE,
                     base::BindOnce(
                         &DedicatedWorkerMessagingProxy::DidFailToFetchScript,
                         weak_factory_.GetWeakPtr()));
      return;
    }
    LocalSchemeFetch::Answer answer = LocalSchemeFetch::AnswerBlobUrl(
        url, std::move(entry->blob), "GET", std::nullopt);
    response_started_ = true;
    response_.url = url;
    response_.status = answer.head.status;
    response_.headers = std::move(answer.head.headers);
    response_.body = std::move(answer.body);
    PageWorkerStarted started;
    started.creator = context.ContextId();
    started.source = PageWorkerSource::kBlob;
    started.module = options_.module;
    started.url = url.spec();
    StartThread(std::move(started));
    // As the browser's loader hands it over, from a task of its own.
    EventLoop::Get()
        ->TaskRunner(TaskType::kInternalLoading)
        ->PostTask(
            FROM_HERE,
            base::BindOnce(&DedicatedWorkerMessagingProxy::OnResponseEnded,
                           weak_factory_.GetWeakPtr(), /*net_error=*/0));
    return;
  }
  if (!url.SchemeIsHTTPOrHTTPS()) {
    EventLoop::Get()
        ->TaskRunner(TaskType::kInternalLoading)
        ->PostTask(
            FROM_HERE,
            base::BindOnce(&DedicatedWorkerMessagingProxy::DidFailToFetchScript,
                           weak_factory_.GetWeakPtr()));
    return;
  }
  // The browser's WorkerScriptFetcher: destination worker, mode same-origin,
  // the credentials of `new Worker()`'s options, and the entry that
  // WorkerMainScriptLoader adds to the creator's Performance Timeline.
  PageRequest request;
  request.kind = PageRequestKind::kWorker;
  request.url = url.spec();
  switch (options.credentials) {
    case FetchCredentialsMode::kOmit:
      request.credentials = PageCredentialsMode::kOmit;
      break;
    case FetchCredentialsMode::kSameOrigin:
      break;
    case FetchCredentialsMode::kInclude:
      request.credentials = PageCredentialsMode::kInclude;
      break;
  }
  script_request_ =
      creator_.requests->StartWorkerScript(std::move(request), *this);
  script_requested_ = true;
}

DedicatedWorkerMessagingProxy::~DedicatedWorkerMessagingProxy() {
  if (script_requested_) {
    creator_.requests->Cancel(script_request_);
  }
  // The thread ends before this returns, stopping a script that still runs.
  thread_.reset();
}

void DedicatedWorkerMessagingProxy::PostMessageToWorkerGlobalScope(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> ports,
    bool user_activation) {
  if (terminated_) {
    return;
  }
  if (!evaluated_ || !thread_) {
    queued_messages_.emplace_back(std::move(message), std::move(ports),
                                  user_activation);
    return;
  }
  thread_->PostMessageToWorkerGlobalScope(std::move(message), std::move(ports),
                                          user_activation);
}

void DedicatedWorkerMessagingProxy::Terminate() {
  if (terminated_) {
    return;
  }
  terminated_ = true;
  queued_messages_.clear();
  if (script_requested_) {
    // The worker never starts, and its Worker hears nothing more.
    script_requested_ = false;
    creator_.requests->Cancel(script_request_);
  }
  if (!thread_) {
    return;
  }
  thread_->Terminate();
  // WorkerThread::ScheduleToTerminateScriptExecution().
  EventLoop::Get()
      ->TaskRunner(TaskType::kInternalDefault)
      ->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<DedicatedWorkerMessagingProxy> proxy) {
                if (proxy && proxy->thread_) {
                  proxy->thread_->TerminateExecution();
                }
              },
              weak_factory_.GetWeakPtr()),
          kForcibleTerminationDelay);
}

void DedicatedWorkerMessagingProxy::OnResponseStarted(
    const PageResponseStarted& response) {
  if (terminated_) {
    return;
  }
  response_started_ = true;
  response_.url = GURL(response.url);
  response_.status = response.status;
  response_.headers = response.headers;
  // DedicatedWorkerHost::DidStartScriptLoad(): the worker starts, and the
  // Session hears of it before anything that it sends.
  PageWorkerStarted started;
  started.creator = context_ ? context_->ContextId() : kNoDocument;
  started.source = PageWorkerSource::kScriptResponse;
  started.module = options_.module;
  started.request = script_request_;
  StartThread(std::move(started));
}

void DedicatedWorkerMessagingProxy::StartThread(PageWorkerStarted started) {
  params_.id =
      creator_.sender->SendContext(started.creator, [&started](uint64_t id) {
        started.worker = id;
        return EncodeWorkerStarted(started);
      });
  params_.object_proxy =
      std::make_unique<ObjectProxy>(weak_factory_.GetWeakPtr());
  thread_ = std::make_unique<DedicatedWorkerThread>(std::move(params_));
}

void DedicatedWorkerMessagingProxy::OnResponseBody(std::string bytes) {
  const size_t size = bytes.size();
  response_.body += bytes;
  creator_.requests->TookBody(script_request_, size);
}

void DedicatedWorkerMessagingProxy::OnResponseEnded(int net_error) {
  script_requested_ = false;
  if (terminated_) {
    return;
  }
  if (!response_started_) {
    // DedicatedWorker::OnScriptLoadStartFailed(): no thread ever started, and
    // the Worker hears nothing more.
    terminated_ = true;
    RunWithWorker(
        base::BindOnce([](Worker& worker) { worker.DidFailToFetchScript(); }));
    if (Worker* const worker = worker_.Get()) {
      worker->DidEnd();
    }
    return;
  }
  response_.net_error = net_error;
  thread_->RunClassicScript(std::move(response_));
}

void DedicatedWorkerMessagingProxy::DispatchMessage(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> ports) {
  if (terminated_) {
    return;
  }
  RunWithWorker(base::BindOnce(
      [](SerializedScriptValue message, std::vector<MessagePortChannel> ports,
         Worker& worker) {
        worker.DispatchMessage(std::move(message), std::move(ports));
      },
      std::move(message), std::move(ports)));
}

void DedicatedWorkerMessagingProxy::DispatchErrorEvent(ErrorMessageInfo error) {
  // Errors that the worker reported still reach the Worker after
  // terminate(), as the HTML Standard has it.
  RunWithWorker(base::BindOnce(
      [](DedicatedWorkerMessagingProxy* proxy, ErrorMessageInfo error,
         Worker& worker) {
        v8::Isolate* const isolate = v8::Isolate::GetCurrent();
        // A new ErrorEvent of the worker's, whose error is null.
        ErrorEvent* const event = ErrorEvent::CreateForScriptError(
            error.message, error.filename, error.line, error.column,
            v8::Null(isolate));
        event->SetTrusted(true);
        if (!worker.DispatchErrorEvent(event)) {
          return;
        }
        // Not canceled: the same event, at the creator's global object, and
        // then its console, which repeats this one level up for a worker.
        ExecutionContext* const context = proxy->context_.Get();
        if (context && !context->ErrorEventTarget()->DispatchEvent(event)) {
          return;
        }
        if (proxy->creator_.report_uncaught) {
          proxy->creator_.report_uncaught.Run(error);
        }
      },
      base::Unretained(this), std::move(error)));
}

void DedicatedWorkerMessagingProxy::AddResourceTiming(
    ResourceTimingInfo info,
    std::u16string initiator_type) {
  ExecutionContext* const context = context_.Get();
  RunInContext(context, [&] {
    context->Performance()->AddResourceTiming(std::move(info), initiator_type);
  });
}

void DedicatedWorkerMessagingProxy::DidEvaluateScript() {
  evaluated_ = true;
  // The messages that waited go to the worker, in order, even if its script
  // threw.
  while (!queued_messages_.empty() && thread_ && !terminated_) {
    QueuedMessage queued = std::move(queued_messages_.front());
    queued_messages_.pop_front();
    thread_->PostMessageToWorkerGlobalScope(std::move(queued.message),
                                            std::move(queued.ports),
                                            queued.user_activation);
  }
}

void DedicatedWorkerMessagingProxy::DidFailToFetchScript() {
  if (terminated_) {
    return;
  }
  RunWithWorker(
      base::BindOnce([](Worker& worker) { worker.DidFailToFetchScript(); }));
}

void DedicatedWorkerMessagingProxy::TerminateFromWorker() {
  // TerminateGlobalScope(), as close() asks: the Worker hears nothing more.
  Terminate();
}

void DedicatedWorkerMessagingProxy::WorkerThreadTerminated() {
  terminated_ = true;
  thread_.reset();
  // After everything that the worker's thread posted on this queue, its
  // errors among it.
  if (Worker* const worker = worker_.Get()) {
    worker->DidEnd();
  }
}

void DedicatedWorkerMessagingProxy::RunWithWorker(
    base::OnceCallback<void(Worker&)> run) {
  Worker* const worker = worker_.Get();
  if (!worker) {
    return;
  }
  RunInContext(context_.Get(), [&] { std::move(run).Run(*worker); });
}

PageWorkerStarter::PageWorkerStarter(
    DedicatedWorkerMessagingProxy::Creator creator)
    : creator_(std::move(creator)) {}

PageWorkerStarter::~PageWorkerStarter() = default;

std::unique_ptr<WorkerMessagingProxy> PageWorkerStarter::StartWorker(
    Worker& worker,
    const GURL& url,
    const WorkerStartOptions& options) {
  return std::make_unique<DedicatedWorkerMessagingProxy>(worker, creator_, url,
                                                         options);
}

}  // namespace mitto
