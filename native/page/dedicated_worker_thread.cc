#include "mitto/page/dedicated_worker_thread.h"

#include <stddef.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/task_observer.h"
#include "base/time/time.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_dedicated_worker_global_scope.h"
#include "mitto/dom/animation_frame_controller.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/classic_pending_script.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/dedicated_worker_global_scope.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/local_scheme_fetch.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/module_script.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/referrer_script_info.h"
#include "mitto/dom/script_fetch_options.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "mitto/page/dedicated_worker_messaging_proxy.h"
#include "mitto/page/page_fetcher.h"
#include "mitto/page/page_isolate.h"
#include "mitto/page/page_platform.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/session_cache_storage.h"
#include "mitto/page/session_indexed_db.h"
#include "mitto/page/session_storage_buckets.h"
#include "mitto/page/worker_scheduler.h"
#include "url/url_constants.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {

namespace {

// A request's priority of 1 and net::HIGHEST, as the messages carry it.
constexpr uint8_t kHighestPriority = 6;
// A response that is used as it arrives, as the messages say it.
constexpr uint8_t kIncremental = 2;

// The stack of a worker's thread, beyond the 500 KB that V8 may use of it.
constexpr size_t kWorkerThreadStackSize = 4 * 1024 * 1024;

}  // namespace

DedicatedWorkerThread::Params::Params() = default;
DedicatedWorkerThread::Params::Params(Params&&) = default;
DedicatedWorkerThread::Params& DedicatedWorkerThread::Params::operator=(
    Params&&) = default;
DedicatedWorkerThread::Params::~Params() = default;

DedicatedWorkerThread::ScriptResponse::ScriptResponse() = default;
DedicatedWorkerThread::ScriptResponse::ScriptResponse(ScriptResponse&&) =
    default;
DedicatedWorkerThread::ScriptResponse&
DedicatedWorkerThread::ScriptResponse::operator=(ScriptResponse&&) = default;
DedicatedWorkerThread::ScriptResponse::~ScriptResponse() = default;

// What lives on the worker's thread while it runs: its isolate, the context
// of its global scope, and what the Page gives that context.
class DedicatedWorkerThread::Scope final : public base::TaskObserver,
                                           public ImportedScriptLoader {
 public:
  Scope(DedicatedWorkerThread& thread,
        Params params,
        WorkerScheduler& scheduler,
        base::OnceClosure quit)
      : thread_(thread),
        params_(std::move(params)),
        scheduler_(scheduler),
        quit_(std::move(quit)) {}

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  ~Scope() override;

  // WorkerThread::InitializeOnWorkerThread(): the isolate, as
  // WorkerBackingThread makes it, then the global scope and its context, as
  // WorkerOrWorkletScriptController::Initialize() makes it.
  void Start();

  // Runs the worker's script, as a classic script or, for a module worker,
  // as the root of its module graph.
  void RunClassicScript(ScriptResponse response);
  void ReceiveMessage(SerializedScriptValue message,
                      std::vector<MessagePortChannel> ports,
                      bool user_activation);
  void RunModuleScript(ScriptResponse response);
  void OnModuleTreeFetched(ModuleScript* module_script);
  // PrepareForShutdownOnWorkerThread(): the context goes, which ends the
  // worker's own workers, and every queue shuts down. Then the thread's loop
  // ends.
  void Shutdown();

  // ImportedScriptLoader:
  std::optional<ImportedScript> Load(const GURL& url) override;

  // base::TaskObserver:
  void WillProcessTask(const base::PendingTask& pending_task,
                       bool was_blocked_or_low_priority) override {}
  // WorkerThread::DidProcessTask(): a microtask checkpoint, the promises
  // rejected with no handler, and the end of a worker that closed.
  void DidProcessTask(const base::PendingTask& pending_task) override;

 private:
  static void OnMicrotasksCompleted(v8::Isolate* isolate, void* data);
  void QueueRejectionReport();
  void ReportRejections(std::vector<PageReporter::Rejection> rejections);
  // An error that no listener of the global scope canceled goes to the
  // Worker, as WorkerGlobalScope::ExceptionThrown() reports it, with what its
  // ErrorEvent said.
  void ReportUncaught(v8::Local<v8::Message> message,
                      v8::Local<v8::Value> exception,
                      const std::u16string& text);
  void OnResponse(FrameFromSession frame);

  const raw_ref<DedicatedWorkerThread> thread_;
  Params params_;
  const raw_ref<WorkerScheduler> scheduler_;
  base::OnceClosure quit_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  std::unique_ptr<bindings::PerIsolateData> data_;
  std::unique_ptr<PageReporter> reporter_;
  v8::Global<v8::Context> context_;
  cppgc::Persistent<DedicatedWorkerGlobalScope> scope_;
  std::unique_ptr<DOMTimerCoordinator> timers_;
  std::unique_ptr<WindowTasks> tasks_;
  std::unique_ptr<AnimationFrameController> animation_frames_;
  std::unique_ptr<PageRequests> requests_;
  std::unique_ptr<PageFetcher> fetcher_;
  std::unique_ptr<SessionCacheStorage> cache_storage_;
  std::unique_ptr<SessionIndexedDb> indexed_db_;
  std::unique_ptr<SessionStorageBuckets> storage_buckets_;
  std::unique_ptr<PageWorkerStarter> worker_starter_;
  int reports_queued_ = 0;
  bool shut_down_ = false;
  base::WeakPtrFactory<Scope> weak_factory_{this};
};

namespace {

// WorkerModuleTreeClient: hears that a module worker's graph has been
// fetched and linked, on the worker's thread.
class WorkerModuleTreeClient final : public ModuleTreeClient {
 public:
  explicit WorkerModuleTreeClient(
      base::OnceCallback<void(ModuleScript* module_script)> finished)
      : finished_(std::move(finished)) {}

  void Trace(cppgc::Visitor* visitor) const override {
    ModuleTreeClient::Trace(visitor);
  }

  void NotifyModuleTreeLoadFinished(ModuleScript* module_script) override {
    if (finished_) {
      std::move(finished_).Run(module_script);
    }
  }

 private:
  base::OnceCallback<void(ModuleScript* module_script)> finished_;
};

}  // namespace

void DedicatedWorkerThread::Scope::Start() {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared =
      std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  params.cpp_heap =
      v8::CppHeap::Create(PagePlatform::Get(), v8::CppHeapCreateParams({}))
          .release();
  ConfigureWorkerIsolate(params, params_.start.memory);
  // V8 asks for the isolate's task runner as soon as it sets the isolate up,
  // which is this thread's.
  isolate_ = v8::Isolate::Allocate();
  PagePlatform::Get()->RegisterIsolate(isolate_, scheduler_->V8TaskRunner());
  v8::Isolate::Initialize(isolate_, params);
  {
    const base::AutoLock lock(thread_->lock_);
    thread_->isolate_ = isolate_;
  }
  isolate_->SetMicrotasksPolicy(v8::MicrotasksPolicy::kScoped);
  InitializeWorkerIsolate(isolate_);
  isolate_->Enter();
  data_ = bindings::PerIsolateData::Create(isolate_, params_.features);
  data_->set_worker();
  const SendFrame send =
      base::BindRepeating(&SessionSender::Send, params_.sender);
  reporter_ = std::make_unique<PageReporter>(isolate_, send);
  reporter_->SetForWorker();
  reporter_->SetUncaughtSink(
      base::BindRepeating(&Scope::ReportUncaught, base::Unretained(this)));
  data_->SetExceptionReporter(base::BindRepeating(
      &PageReporter::ReportException, base::Unretained(reporter_.get())));
  data_->SetKnownGapReporter(base::BindRepeating(
      [](const SendFrame& send, std::string_view name) {
        send.Run(EncodeKnownGap(name));
      },
      send));

  const v8::HandleScope handle_scope(isolate_);
  ContextInit init;
  // A worker's performance timeline starts when its thread does.
  init.time_origin = base::TimeTicks::Now();
  init.secure_context = params_.secure_context;
  init.cross_origin_isolated = params_.cross_origin_isolated;
  const v8::Local<v8::Context> context = NewWorkerContext(
      isolate_, init, params_.name, [this](v8::Local<v8::Context> context) {
        InitializePageContext(context);
        reporter_->ContextCreated(context);
      });
  context_.Reset(isolate_, context);
  const v8::Context::Scope context_scope(context);
  scope_ = bindings::V8DedicatedWorkerGlobalScope::ToWrappable(
      isolate_, context->Global());
  CHECK(scope_);
  scope_->SetOrigin(params_.origin);
  scope_->SetStart(params_.start, params_.connection);
  scope_->SetContextId(params_.id);
  timers_ = std::make_unique<DOMTimerCoordinator>(isolate_, context);
  scope_->SetTimers(timers_.get());
  tasks_ = std::make_unique<WindowTasks>(isolate_, context);
  scope_->SetTasks(tasks_.get());
  if (params_.begin_frames) {
    animation_frames_ = std::make_unique<AnimationFrameController>(
        isolate_, context, init.time_origin);
    scope_->SetAnimationFrames(animation_frames_.get());
  }
  // The worker's requests are its own, and their responses come to this
  // thread, on its networking queue.
  requests_ = std::make_unique<PageRequests>(
      params_.sender, params_.refuse,
      base::BindRepeating(
          [](scoped_refptr<base::SingleThreadTaskRunner> networking,
             Scope* scope, FrameFromSession frame) {
            networking->PostTask(
                FROM_HERE,
                base::BindOnce(&Scope::OnResponse, base::Unretained(scope),
                               std::move(frame)));
          },
          scheduler_->TaskRunner(TaskType::kNetworking),
          base::Unretained(this)));
  fetcher_ = std::make_unique<PageFetcher>(*requests_);
  scope_->SetFetcher(fetcher_.get());
  // The worker's own Cache API, IndexedDB and Storage Buckets, as its
  // creator's frame's storage key reaches them, unless its origin is opaque
  // and may not.
  if (!params_.origin.opaque()) {
    cache_storage_ = std::make_unique<SessionCacheStorage>();
    cache_storage_->SetSender(
        params_.sender, scheduler_->TaskRunner(TaskType::kMiscPlatformAPI),
        params_.refuse);
    cache_storage_->DocumentCommitted(params_.id);
    scope_->SetCacheStorageHost(cache_storage_.get());
    indexed_db_ = std::make_unique<SessionIndexedDb>();
    indexed_db_->SetSender(params_.sender,
                           scheduler_->TaskRunner(TaskType::kDatabaseAccess),
                           params_.refuse);
    indexed_db_->DocumentCommitted(params_.id);
    scope_->SetIndexedDbHost(indexed_db_.get());
    storage_buckets_ = std::make_unique<SessionStorageBuckets>();
    storage_buckets_->SetSender(
        params_.sender, scheduler_->TaskRunner(TaskType::kMiscPlatformAPI),
        params_.refuse);
    storage_buckets_->DocumentCommitted(params_.id);
    scope_->SetStorageBucketHost(storage_buckets_.get());
  }
  scope_->SetConsoleSink(base::BindRepeating(
      [](const SendFrame& send, ConsoleLevel level, std::string text) {
        PageConsoleMessage message;
        message.level = level;
        message.text = std::move(text);
        send.Run(EncodeConsole(message));
      },
      send));
  scope_->SetObjectProxy(params_.object_proxy.get());
  scope_->SetModule(params_.module);
  scope_->SetOutsideReferrerPolicy(params_.referrer_policy);
  scope_->SetOutsideOrigin(params_.outside_origin);
  scope_->SetImportedScriptLoader(this);
  // The worker's own workers, whose errors that nothing canceled go on to
  // this worker's Worker.
  DedicatedWorkerMessagingProxy::Creator creator(*requests_);
  creator.sender = params_.sender;
  creator.refuse = params_.refuse;
  creator.report_uncaught = base::BindRepeating(
      [](DedicatedWorkerObjectProxy* proxy, const ErrorMessageInfo& error) {
        proxy->ReportException(error.message, error.filename, error.line,
                               error.column);
      },
      base::Unretained(params_.object_proxy.get()));
  worker_starter_ = std::make_unique<PageWorkerStarter>(std::move(creator));
  scope_->SetWorkerStarter(worker_starter_.get());
  base::CurrentThread::Get()->AddTaskObserver(this);
  isolate_->AddMicrotasksCompletedCallback(&Scope::OnMicrotasksCompleted, this);
}

DedicatedWorkerThread::Scope::~Scope() {
  if (!isolate_) {
    return;
  }
  Shutdown();
  base::CurrentThread::Get()->RemoveTaskObserver(this);
  isolate_->RemoveMicrotasksCompletedCallback(&Scope::OnMicrotasksCompleted,
                                              this);
  {
    const v8::HandleScope handle_scope(isolate_);
    if (!context_.IsEmpty()) {
      reporter_->ContextDestroyed(context_.Get(isolate_));
    }
  }
  scope_.Clear();
  context_.Reset();
  worker_starter_.reset();
  // What the Cache API, IndexedDB and Storage Buckets wait to hear holds the
  // isolate's handles, so they go before it does.
  cache_storage_.reset();
  indexed_db_.reset();
  storage_buckets_.reset();
  fetcher_.reset();
  requests_.reset();
  animation_frames_.reset();
  tasks_.reset();
  timers_.reset();
  reporter_.reset();
  data_.reset();
  {
    const base::AutoLock lock(thread_->lock_);
    thread_->isolate_ = nullptr;
  }
  isolate_->Exit();
  PagePlatform::Get()->UnregisterIsolate(isolate_);
  isolate_->Dispose();
  isolate_ = nullptr;
  // The Session forgets the worker, after everything that it sent.
  params_.sender->SendContextGone(params_.id);
  params_.object_proxy->DidTerminateWorkerThread();
}

void DedicatedWorkerThread::Scope::Shutdown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  if (scope_) {
    const v8::HandleScope handle_scope(isolate_);
    const v8::Context::Scope context_scope(context_.Get(isolate_));
    // NotifyContextDestroyed(): what goes with the context goes, the
    // worker's own workers among it.
    scope_->SetTimers(nullptr);
    scope_->SetTasks(nullptr);
    scope_->SetFetcher(nullptr);
    scope_->SetWorkerStarter(nullptr);
    scope_->SetImportedScriptLoader(nullptr);
    scope_->SetCacheStorageHost(nullptr);
    scope_->SetIndexedDbHost(nullptr);
    scope_->SetStorageBucketHost(nullptr);
    scope_->SetAnimationFrames(nullptr);
  }
  // The fetcher lets its keepalive requests go before the rest are cancelled,
  // so that they go on without the worker, as a window's do without their
  // document.
  fetcher_.reset();
  if (requests_) {
    requests_->CancelAll();
  }
  scheduler_->Shutdown();
  if (quit_) {
    std::move(quit_).Run();
  }
}

void DedicatedWorkerThread::Scope::RunClassicScript(ScriptResponse response) {
  if (shut_down_) {
    return;
  }
  if (params_.module) {
    RunModuleScript(std::move(response));
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  // WorkerClassicScriptLoader::DidReceiveResponse(): a response that is not
  // 2xx, or whose MIME type is not one to run, is a failure, after the
  // worker's thread has started.
  const bool ok = response.net_error == 0 &&
                  (response.status == 0 ||
                   (response.status >= 200 && response.status < 300));
  const std::optional<std::string> refused =
      ok ? RefusedAsWorkerScript(response.url, response.headers,
                                 params_.strict_mime_type)
         : std::nullopt;
  if (refused) {
    scope_->AddConsoleMessage(ConsoleLevel::kError, *refused);
  }
  if (!ok || refused) {
    // WorkerGlobalScope::DidFetchClassicScript(): the Worker hears of it, and
    // the worker closes.
    params_.object_proxy->DidFailToFetchScript();
    scope_->Close();
    return;
  }
  // InitializeURL(): the worker's URL is its script's final one.
  scope_->SetUrl(response.url);
  // WorkerClassicScriptLoader decodes as UTF-8, unless the response's charset
  // or a byte order mark names another.
  const std::unique_ptr<TextResourceDecoder> decoder =
      BuildClassicScriptDecoder(
          "UTF-8",
          GetMimeTypeAndCharset(HeaderField(response.headers, "content-type"))
              .charset);
  std::u16string source = decoder->Decode(base::as_byte_span(response.body));
  source += decoder->Flush();
  // EvaluateClassicScript(): named after the response's URL, its errors as
  // they are.
  {
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::String> name;
    v8::Local<v8::String> code;
    v8::Local<v8::Script> script;
    if (v8::String::NewFromUtf8(isolate_, response.url.spec().c_str())
            .ToLocal(&name) &&
        v8::String::NewFromTwoByte(
            isolate_, reinterpret_cast<const uint16_t*>(source.data()),
            v8::NewStringType::kNormal, base::checked_cast<int>(source.size()))
            .ToLocal(&code)) {
      // A classic worker script's base URL, which its import() resolves
      // against, is its response's, with the default fetch options.
      v8::ScriptOrigin origin = ClassicScriptOrigin(
          name, /*muted_errors=*/false, /*line_offset=*/0,
          /*column_offset=*/0,
          ReferrerScriptInfo(response.url, ScriptFetchOptions())
              .ToV8HostDefinedOptions(isolate_, response.url));
      v8::ScriptCompiler::Source script_source(code, origin);
      if (v8::ScriptCompiler::Compile(context, &script_source)
              .ToLocal(&script)) {
        std::ignore = script->Run(context);
      }
    }
    ReportScriptException(isolate_, try_catch);
  }
  // DidEvaluateTopLevelScript(): the messages that waited for it go to the
  // worker.
  params_.object_proxy->DidEvaluateTopLevelScript();
}

// WorkerGlobalScope::FetchModuleTopLevelScript(): the graph of the worker's
// script, whose response its creator took, with the destination worker and
// the credentials of new Worker()'s options.
void DedicatedWorkerThread::Scope::RunModuleScript(ScriptResponse response) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  // WorkerModuleScriptFetcher: a response that failed or is not ok gives no
  // module script.
  if (response.net_error != 0 ||
      (response.status != 0 &&
       (response.status < 200 || response.status > 299))) {
    OnModuleTreeFetched(nullptr);
    return;
  }
  // InitializeURL(): the worker's URL is its script's final one.
  scope_->SetUrl(response.url);
  auto main_script = std::make_shared<ModuleWorkerMainScript>();
  main_script->url = response.url;
  // A data: URL's, which no request answered.
  main_script->status = response.status == 0 ? 200 : response.status;
  main_script->headers = std::move(response.headers);
  main_script->body = std::move(response.body);
  ScriptFetchOptions options;
  options.credentials_mode = params_.credentials;
  scope_->GetModulator()->FetchWorkerScriptTree(
      params_.script_url, std::move(main_script), options,
      bindings::MakeGarbageCollected<WorkerModuleTreeClient>(base::BindOnce(
          &Scope::OnModuleTreeFetched, weak_factory_.GetWeakPtr())));
}

// WorkerModuleTreeClient::NotifyModuleTreeLoadFinished(), then
// RunWorkerScript().
void DedicatedWorkerThread::Scope::OnModuleTreeFetched(
    ModuleScript* module_script) {
  if (shut_down_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  // A graph that failed, or whose root has an error to rethrow, a parse or
  // link error: the Worker hears a plain error, and the worker closes.
  if (!module_script || module_script->HasErrorToRethrow()) {
    params_.object_proxy->DidFailToFetchScript();
    scope_->Close();
    return;
  }
  {
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    // What the evaluation throws, or a rejection of its promise, is reported
    // as any error that the worker leaves uncaught.
    module_script->RunScript();
  }
  params_.object_proxy->DidEvaluateTopLevelScript();
}

void DedicatedWorkerThread::Scope::ReceiveMessage(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> ports,
    bool user_activation) {
  if (shut_down_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  // Each listener's microtasks run as it returns, and the rest at the task's
  // end.
  const v8::MicrotasksScope microtasks_scope(
      context_.Get(isolate_), v8::MicrotasksScope::kDoNotRunMicrotasks);
  scope_->ReceiveMessage(std::move(message), std::move(ports), user_activation);
}

void DedicatedWorkerThread::Scope::OnResponse(FrameFromSession frame) {
  if (shut_down_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  switch (frame.type) {
    case PageMessage::kResponseStarted:
      requests_->OnResponseStarted(frame.response);
      return;
    case PageMessage::kResponseBody:
      requests_->OnResponseBody(std::move(frame.body));
      return;
    case PageMessage::kResponseEnded:
      requests_->OnResponseEnded(frame.ended);
      return;
    case PageMessage::kRequestBodyCredit:
      requests_->OnRequestBodyCredit(frame.request_body_credit);
      return;
    default:
      NOTREACHED();
  }
}

// What importScripts() hears of one script's request.
class ImportedScriptClient final : public PageRequests::Client {
 public:
  ImportedScriptClient() = default;
  ImportedScriptClient(const ImportedScriptClient&) = delete;
  ImportedScriptClient& operator=(const ImportedScriptClient&) = delete;
  ~ImportedScriptClient() override = default;

  // PageRequests::Client:
  void OnResponseStarted(const PageResponseStarted& response) override {
    script_.url = GURL(response.url);
    script_.status = response.status;
    script_.headers = response.headers;
    script_.opaque = response.opaque;
    started_ = true;
  }
  void OnResponseBody(std::string bytes) override { body_ += bytes; }
  void OnResponseEnded(int net_error) override {
    net_error_ = net_error;
    ended_ = true;
  }

  // The script, once its response has ended with all of its body.
  std::optional<ImportedScript> Take() {
    if (!started_ || !ended_ || net_error_ != 0) {
      return std::nullopt;
    }
    script_.body = std::move(body_);
    return std::move(script_);
  }

 private:
  ImportedScript script_;
  std::string body_;
  int net_error_ = 0;
  bool started_ = false;
  bool ended_ = false;
};

std::optional<ImportedScript> DedicatedWorkerThread::Scope::Load(
    const GURL& url) {
  if (shut_down_) {
    return std::nullopt;
  }
  // A data: URL's script never reaches the network.
  if (url.SchemeIs(url::kDataScheme)) {
    std::optional<DataUrlResponse> data = ParseDataUrl(url);
    if (!data) {
      return std::nullopt;
    }
    ImportedScript script;
    script.url = url;
    script.status = 200;
    script.headers = "Content-Type: " + data->content_type + "\n";
    script.body = std::move(data->body);
    return script;
  }
  // A blob: URL's comes from the blob it names, of the worker's origin, as
  // the browser's BlobURLLoader reads it.
  if (url.SchemeIsBlob()) {
    std::optional<BlobUrlRegistry::Entry> entry =
        BlobUrlRegistry::Get().ResolveEntry(url);
    if (!entry ||
        !entry->origin.IsSameOriginWith(scope_->GetSecurityOrigin())) {
      return std::nullopt;
    }
    LocalSchemeFetch::Answer answer = LocalSchemeFetch::AnswerBlobUrl(
        url, std::move(entry->blob), "GET", std::nullopt);
    if (answer.net_error) {
      return std::nullopt;
    }
    ImportedScript script;
    script.url = url;
    script.status = answer.head.status;
    script.headers = std::move(answer.head.headers);
    script.body = answer.TakeBody();
    return script;
  }
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return std::nullopt;
  }
  // WorkerClassicScriptLoader::LoadSynchronously(): destination script, of
  // mode no-cors and credentials include, which a script's Request Kind has,
  // at the highest priority, as Blink gives a synchronous request. It loads
  // as a RawResource, which ResourceFetcher marks incremental.
  PageRequest request;
  request.kind = PageRequestKind::kScript;
  request.url = url.spec();
  request.priority = kHighestPriority;
  request.incremental = kIncremental;
  ImportedScriptClient client;
  requests_->StartSynchronous(std::move(request), client,
                              thread_->abort_synchronous_load());
  return client.Take();
}

void DedicatedWorkerThread::Scope::DidProcessTask(
    const base::PendingTask& pending_task) {
  if (shut_down_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  v8::MicrotasksScope::PerformCheckpoint(isolate_);
  QueueRejectionReport();
  // A worker that closed ends once its task has.
  if (scope_->IsClosing()) {
    params_.object_proxy->DidCloseWorkerGlobalScope();
    Shutdown();
  }
}

// static
void DedicatedWorkerThread::Scope::OnMicrotasksCompleted(v8::Isolate* isolate,
                                                         void* data) {
  static_cast<Scope*>(data)->QueueRejectionReport();
}

void DedicatedWorkerThread::Scope::QueueRejectionReport() {
  if (shut_down_) {
    return;
  }
  // As RejectedPromises::ProcessQueue() on a worker: from a task of its own.
  std::vector<PageReporter::Rejection> rejections =
      reporter_->TakeUnhandledRejections();
  if (rejections.empty()) {
    return;
  }
  ++reports_queued_;
  scheduler_->TaskRunner(TaskType::kDOMManipulation)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&Scope::ReportRejections,
                                base::Unretained(this), std::move(rejections)));
}

void DedicatedWorkerThread::Scope::ReportRejections(
    std::vector<PageReporter::Rejection> rejections) {
  --reports_queued_;
  if (shut_down_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  reporter_->ReportUnhandledRejections(std::move(rejections));
}

void DedicatedWorkerThread::Scope::ReportUncaught(
    v8::Local<v8::Message> message,
    v8::Local<v8::Value> exception,
    const std::u16string& text) {
  if (!scope_ || message.IsEmpty()) {
    return;
  }
  ErrorMessageInfo info = DescribeErrorMessage(isolate_, *scope_, message);
  params_.object_proxy->ReportException(text, std::move(info.filename),
                                        info.line, info.column);
}

DedicatedWorkerThread::DedicatedWorkerThread(Params params)
    : scheduler_(std::make_unique<WorkerScheduler>()),
      params_(std::move(params)),
      script_runner_(scheduler_->TaskRunner(TaskType::kDOMManipulation)),
      message_runner_(scheduler_->TaskRunner(TaskType::kPostedMessage)),
      control_runner_(scheduler_->ControlTaskRunner()) {
  CHECK(base::PlatformThread::CreateWithType(
      kWorkerThreadStackSize, this, &thread_, base::ThreadType::kDefault));
}

DedicatedWorkerThread::~DedicatedWorkerThread() {
  Terminate();
  TerminateExecution();
  base::PlatformThread::Join(thread_);
}

void DedicatedWorkerThread::ThreadMain() {
  base::PlatformThread::SetName("DedicatedWorker thread");
  std::unique_ptr<WorkerScheduler> scheduler = std::move(scheduler_);
  scheduler->BindToCurrentThread();
  {
    base::RunLoop run_loop;
    Scope scope(*this, std::move(params_), *scheduler, run_loop.QuitClosure());
    scope_ = &scope;
    scope.Start();
    run_loop.Run();
    scope_ = nullptr;
  }
  scheduler.reset();
}

void DedicatedWorkerThread::RunClassicScript(ScriptResponse response) {
  // WorkerThread::FetchAndRunClassicScript() posts to the worker's DOM
  // manipulation queue.
  script_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](DedicatedWorkerThread* thread, ScriptResponse response) {
            if (thread->scope_) {
              thread->scope_->RunClassicScript(std::move(response));
            }
          },
          base::Unretained(this), std::move(response)));
}

void DedicatedWorkerThread::PostMessageToWorkerGlobalScope(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> ports,
    bool user_activation) {
  message_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](DedicatedWorkerThread* thread, SerializedScriptValue message,
             std::vector<MessagePortChannel> ports, bool user_activation) {
            if (thread->scope_) {
              thread->scope_->ReceiveMessage(std::move(message),
                                             std::move(ports), user_activation);
            }
          },
          base::Unretained(this), std::move(message), std::move(ports),
          user_activation));
}

void DedicatedWorkerThread::Terminate() {
  abort_synchronous_load_.Signal();
  {
    const base::AutoLock lock(lock_);
    if (terminating_) {
      return;
    }
    terminating_ = true;
  }
  control_runner_->PostTask(FROM_HERE, base::BindOnce(
                                           [](DedicatedWorkerThread* thread) {
                                             if (thread->scope_) {
                                               thread->scope_->Shutdown();
                                             }
                                           },
                                           base::Unretained(this)));
}

void DedicatedWorkerThread::TerminateExecution() {
  const base::AutoLock lock(lock_);
  if (isolate_) {
    isolate_->TerminateExecution();
  }
}

}  // namespace mitto
