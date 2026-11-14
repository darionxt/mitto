#include "mitto/page/script_runner.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/dom/animation_frame_controller.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/idle_task_controller.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/page/chrome_object.h"
#include "mitto/page/document_loader.h"
#include "mitto/page/page_fetcher.h"
#include "mitto/page/page_isolate.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/session_cookie_jar.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "third_party/icu/source/common/unicode/stringpiece.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-platform.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-value.h"

namespace mitto {
namespace {

// Why a call that waits for a promise fails when its context goes, as
// DevTools says when a page navigates while it waits.
constexpr std::string_view kContextDestroyed =
    "Execution context was destroyed.";

// net::HostStringIsLocalhost(): a loopback address, or the name localhost or
// one under it.
// Upstream: net/base/url_util.cc
bool IsLocalhost(const GURL& url) {
  std::string_view host = url.HostNoBracketsPiece();
  if (url.HostIsIPAddress()) {
    return host.starts_with("127.") || host == "::1";
  }
  if (host.ends_with('.')) {
    host.remove_suffix(1);
  }
  return host == "localhost" || host.ends_with(".localhost");
}

// What v8::JSON::Stringify() hands back when JSON.stringify() writes nothing,
// as for undefined, a function or a symbol. No JSON reads so.
constexpr std::string_view kNothingWritten = "undefined";

constexpr std::string_view kUncaught = "Uncaught ";
// As Chrome's console says of a promise rejected with no handler.
constexpr std::string_view kUncaughtInPromise = "Uncaught (in promise) ";

// Returns why a call failed, from what `try_catch` caught.
std::string Failure(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    const v8::TryCatch& try_catch) {
  if (!try_catch.HasCaught()) {
    return base::StrCat({kUncaught, "exception"});
  }
  return DescribeUncaught(isolate, context, try_catch.Exception(), kUncaught,
                          kMaxEvaluatedTextSize);
}

}  // namespace

// Upstream: gin/v8_initializer.cc
bool GenerateEntropy(unsigned char* buffer, size_t size) {
  // SAFETY: V8 gives a buffer of `size` bytes.
  base::RandBytes(UNSAFE_BUFFERS(base::span(buffer, size)));
  return true;
}

// Upstream: net/base/is_potentially_trustworthy.cc
bool IsUrlPotentiallyTrustworthy(const GURL& url) {
  if (url.IsAboutBlank() || url.IsAboutSrcdoc() ||
      url.SchemeIs(url::kDataScheme)) {
    return true;
  }
  const url::Origin origin = url::Origin::Create(url);
  if (origin.opaque()) {
    return false;
  }
  return GURL::SchemeIsCryptographic(origin.scheme()) || IsLocalhost(url) ||
         origin.scheme() == url::kFileScheme;
}

ScriptRunner::ScriptRunner(v8::Isolate* isolate) : isolate_(isolate) {}

ScriptRunner::~ScriptRunner() {
  if (!context_.IsEmpty()) {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
  }
}

std::optional<std::string> ScriptRunner::Start(
    const PageStart& start,
    SendFrame send,
    base::RepeatingCallback<void(std::string why)> refuse,
    SessionCookieJar& cookie_jar) {
  send_ = std::move(send);
  start_ = start;
  cookie_jar_ = &cookie_jar;
  cookie_jar_->SetSend(send_);
  requests_ = std::make_unique<PageRequests>(send_, std::move(refuse));
  loader_ = std::make_unique<DocumentLoader>(*this, *requests_, send_);
  requests_->SetNavigationClient(*loader_);
  // As Blink sets the time zone that its browser process reads.
  // Upstream: third_party/blink/renderer/core/timezone/timezone_controller.cc
  std::unique_ptr<icu::TimeZone> time_zone(icu::TimeZone::createTimeZone(
      icu::UnicodeString::fromUTF8(start.time_zone)));
  if (!time_zone || *time_zone == icu::TimeZone::getUnknown()) {
    return base::StrCat({"the time zone \"", start.time_zone,
                         "\" is unknown"});
  }
  UErrorCode status = U_ZERO_ERROR;
  const icu::Locale locale = icu::Locale::forLanguageTag(start.locale, status);
  if (U_FAILURE(status) || locale.isBogus()) {
    return base::StrCat(
        {"the locale \"", start.locale, "\" is not a BCP 47 tag"});
  }
  icu::Locale::setDefault(locale, status);
  if (U_FAILURE(status)) {
    return base::StrCat({"the locale \"", start.locale, "\" cannot be set"});
  }
  icu::TimeZone::adoptDefault(time_zone.release());
  // The frames begin at the vsyncs of the Machine's display.
  EventLoop::Get()->SetRefreshRate(start.refresh_rate);
  isolate_->DateTimeConfigurationChangeNotification(
      v8::Isolate::TimeZoneDetection::kSkip);
  isolate_->LocaleConfigurationChangeNotification();
  reporter_ = std::make_unique<PageReporter>(isolate_, send_);
  bindings::PerIsolateData::From(isolate_)->SetExceptionReporter(
      base::BindRepeating(&PageReporter::ReportException,
                          base::Unretained(reporter_.get())));
  // The first time each document reaches a Known Gap, the Session hears of it.
  bindings::PerIsolateData::From(isolate_)->SetKnownGapReporter(
      base::BindRepeating(
          [](const SendFrame& send, std::string_view name) {
            send.Run(EncodeKnownGap(name));
          },
          send_));

  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context =
      NewContext(bindings::PerIsolateData::From(isolate_)->time_origin());
  context_.Reset(isolate_, context);
  AttachWindow(context, start_.connection, PageDocumentSource::kInitialEmpty,
               kNavigationRequest);
  base::CurrentThread::Get()->AddTaskObserver(this);
  return std::nullopt;
}

v8::Local<v8::Context> ScriptRunner::NewContext(base::TimeTicks time_origin) {
  chrome_object_data_.push_back(
      std::make_unique<ChromeObjectData>(time_origin));
  ChromeObjectData& chrome_object_data = *chrome_object_data_.back();
  return NewPageContext(
      isolate_, u"",
      [this, &chrome_object_data](v8::Local<v8::Context> context) {
        // As LocalWindowProxy::Initialize(): V8Initializer's settings of the
        // context, then the inspector, then the embedder.
        InitializePageContext(context);
        reporter_->ContextCreated(context);
        InstallChromeObject(context, chrome_object_data);
      });
}

void ScriptRunner::AttachWindow(v8::Local<v8::Context> context,
                                const PageConnection& connection,
                                PageDocumentSource source,
                                uint64_t navigation) {
  const v8::Context::Scope context_scope(context);
  // The Session hears of the document before anything that it sends.
  PageDocumentCommitted committed;
  committed.document = ++last_document_id_;
  committed.source = source;
  committed.navigation = navigation;
  send_.Run(EncodeDocumentCommitted(committed));
  timers_ = std::make_unique<DOMTimerCoordinator>(isolate_, context);
  idle_tasks_ = std::make_unique<IdleTaskController>(isolate_, context);
  animation_frames_ =
      std::make_unique<AnimationFrameController>(isolate_, context);
  fetcher_ = std::make_unique<PageFetcher>(*requests_);
  window_tasks_ = std::make_unique<WindowTasks>(isolate_, context);
  Window& window = Window::Current();
  window.SetTimers(timers_.get());
  window.SetIdleTasks(idle_tasks_.get());
  window.SetAnimationFrames(animation_frames_.get());
  window.SetFetcher(fetcher_.get());
  window.SetTasks(window_tasks_.get());
  window.SetStart(start_, connection);
  window.SetDocumentId(committed.document);
  window.SetDocumentLoadTiming(chrome_object_data_.back()->load_timing.Get());
  window.SetConsoleSink(base::BindRepeating(
      [](const SendFrame& send, ConsoleLevel level, std::string text) {
        PageConsoleMessage message;
        message.level = level;
        message.text = std::move(text);
        send.Run(EncodeConsole(message));
      },
      send_));
}

void ScriptRunner::WillProcessTask(const base::PendingTask& pending_task,
                                   bool was_blocked_or_low_priority) {}

void ScriptRunner::DidProcessTask(const base::PendingTask& pending_task) {
  EndTask();
}

void ScriptRunner::Evaluate(uint64_t call, std::string_view script) {
  // Start() failed, and the Page is about to end.
  if (context_.IsEmpty()) {
    send_.Run(EncodeEvaluated(FailureOf(call, "the Page did not start")));
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  std::optional<PageEvaluated> evaluated;
  v8::Local<v8::Value> result;
  {
    // The script's microtasks run when it returns, as Blink runs them once a
    // script it ran returns, after the TryCatch has gone, so that what they
    // throw is reported rather than taken for the script's.
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::String> source;
    v8::Local<v8::Script> compiled;
    // As DevTools' EvaluateGlobal() compiles what it evaluates: with no name,
    // and errors that the window's error listeners see as they are.
    v8::ScriptOrigin origin(
        v8::String::Empty(isolate_), /*resource_line_offset=*/0,
        /*resource_column_offset=*/0,
        /*resource_is_shared_cross_origin=*/true, /*script_id=*/-1,
        /*source_map_url=*/v8::Local<v8::Value>(), /*resource_is_opaque=*/true);
    if (!v8::String::NewFromUtf8(isolate_, script.data(),
                                 v8::NewStringType::kNormal,
                                 base::checked_cast<int>(script.size()))
             .ToLocal(&source) ||
        !v8::Script::Compile(context, source, &origin).ToLocal(&compiled) ||
        !compiled->Run(context).ToLocal(&result)) {
      evaluated = FailureOf(call, Failure(isolate_, context, try_catch));
    }
  }
  if (!evaluated) {
    if (result->IsPromise()) {
      // The microtasks of the script have run, so what they settle has
      // settled.
      evaluated = SettledResultOf(call, context, result.As<v8::Promise>());
    } else {
      evaluated = ResultOf(call, context, result);
    }
  }
  if (evaluated) {
    settled_.push_back(*std::move(evaluated));
  }
}

void ScriptRunner::EndTask() {
  if (context_.IsEmpty()) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  // As Blink's scheduler does after each task.
  v8::MicrotasksScope::PerformCheckpoint(isolate_);
  // As Blink's RejectedPromises::ProcessQueue(): the promises that the task
  // left rejected with no handler are reported from a task of their own, so
  // that a handler that a task queued before it adds is in time.
  std::vector<PageReporter::Rejection> rejections =
      reporter_->TakeUnhandledRejections();
  if (!rejections.empty()) {
    ++reports_queued_;
    EventLoop::Get()
        ->TaskRunner(TaskType::kDOMManipulation)
        ->PostTask(FROM_HERE, base::BindOnce(&ScriptRunner::ReportRejections,
                                             base::Unretained(this),
                                             std::move(rejections)));
  }
  // Results wait for the reports queued before them.
  if (reports_queued_ > 0) {
    return;
  }
  std::vector<PageEvaluated> settled;
  settled.swap(settled_);
  for (const PageEvaluated& evaluated : settled) {
    send_.Run(EncodeEvaluated(evaluated));
  }
}

void ScriptRunner::ReportRejections(
    std::vector<PageReporter::Rejection> rejections) {
  --reports_queued_;
  const v8::HandleScope handle_scope(isolate_);
  reporter_->ReportUnhandledRejections(context_.Get(isolate_),
                                       std::move(rejections));
}

void ScriptRunner::OnResponseStarted(PageResponseStarted response) {
  RunTask(base::BindOnce(&PageRequests::OnResponseStarted,
                         base::Unretained(requests_.get()),
                         std::move(response)));
}

void ScriptRunner::OnResponseBody(PageResponseBody body) {
  RunTask(base::BindOnce(&PageRequests::OnResponseBody,
                         base::Unretained(requests_.get()), std::move(body)));
}

void ScriptRunner::OnResponseEnded(PageResponseEnded ended) {
  RunTask(base::BindOnce(&PageRequests::OnResponseEnded,
                         base::Unretained(requests_.get()), ended));
}

void ScriptRunner::RunTask(base::OnceClosure task) {
  // Start() failed, and the Page is about to end.
  if (context_.IsEmpty()) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  std::move(task).Run();
}

void ScriptRunner::PostTask(TaskType type, base::OnceClosure task) {
  // The runner lives until the process ends.
  EventLoop::Get()->TaskRunner(type)->PostTask(
      FROM_HERE, base::BindOnce(&ScriptRunner::RunTask, base::Unretained(this),
                                std::move(task)));
}

Document& ScriptRunner::CommitDocument(uint64_t navigation,
                                       const GURL& url,
                                       base::TimeTicks navigation_start,
                                       const PageConnection& connection) {
  const v8::HandleScope handle_scope(isolate_);
  for (const uint64_t call : waiting_calls_) {
    settled_.push_back(FailureOf(call, std::string(kContextDestroyed)));
  }
  waiting_calls_.clear();
  {
    // The old window's timers and fetches stop with its context.
    const v8::Context::Scope old_scope(context_.Get(isolate_));
    Window::Current().SetTimers(nullptr);
    Window::Current().SetIdleTasks(nullptr);
    Window::Current().SetAnimationFrames(nullptr);
    Window::Current().SetFetcher(nullptr);
    Window::Current().SetTasks(nullptr);
  }
  timers_.reset();
  idle_tasks_.reset();
  animation_frames_.reset();
  fetcher_.reset();
  window_tasks_.reset();
  reporter_->ContextDestroyed(context_.Get(isolate_));
  chrome_object_data_.back()->detached = true;
  // What the new document's DOMHighResTimeStamp counts from, as Blink takes
  // it from the navigation's start.
  const base::TimeTicks now = base::TimeTicks::Now();
  const base::TimeTicks time_origin =
      navigation_start.is_null() || navigation_start > now ? now
                                                           : navigation_start;
  bindings::PerIsolateData::From(isolate_)->set_time_origin(time_origin);
  // A [SecureContext] member is installed when a context is made.
  bindings::PerIsolateData::From(isolate_)->set_secure_context(
      IsUrlPotentiallyTrustworthy(url));
  // Chrome's site isolation locks a renderer to the site of a document at an
  // HTTP or HTTPS URL.
  MemoryInfo::SetProcessLockedToSite(url.SchemeIsHTTPOrHTTPS());
  const v8::Local<v8::Context> context = NewContext(time_origin);
  context_.Reset(isolate_, context);
  AttachWindow(context, connection, PageDocumentSource::kNavigationResponse,
               navigation);
  const v8::Context::Scope context_scope(context);
  Document* const document = Document::CreateHTML();
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  // Only a document that the Session loaded over HTTP or HTTPS has cookies
  // there; another's origin is opaque to them.
  if (url.SchemeIsHTTPOrHTTPS()) {
    document->SetCookieJar(cookie_jar_);
  }
  Window::Current().SetDocument(document);
  return *document;
}

PageEvaluated ScriptRunner::ResultOf(uint64_t call,
                                     v8::Local<v8::Context> context,
                                     v8::Local<v8::Value> value) {
  const v8::TryCatch try_catch(isolate_);
  v8::Local<v8::String> json;
  if (!v8::JSON::Stringify(context, value).ToLocal(&json)) {
    return FailureOf(call, Failure(isolate_, context, try_catch));
  }
  PageEvaluated evaluated;
  evaluated.call = call;
  const v8::String::Utf8Value text(isolate_, json);
  // SAFETY: Utf8Value holds length() bytes at *text.
  evaluated.text =
      UNSAFE_BUFFERS(std::string(*text, static_cast<size_t>(text.length())));
  if (evaluated.text == kNothingWritten) {
    evaluated.text.clear();
  }
  if (evaluated.text.size() > kMaxEvaluatedTextSize) {
    evaluated.failed = true;
    evaluated.text = base::StrCat(
        {"the result is ", base::NumberToString(evaluated.text.size()),
         " bytes of JSON, more than the channel carries"});
  }
  return evaluated;
}

PageEvaluated ScriptRunner::FailureOf(uint64_t call, std::string text) {
  PageEvaluated evaluated;
  evaluated.call = call;
  evaluated.failed = true;
  evaluated.text = std::move(text);
  return evaluated;
}

std::optional<PageEvaluated> ScriptRunner::SettledResultOf(
    uint64_t call,
    v8::Local<v8::Context> context,
    v8::Local<v8::Promise> promise) {
  switch (promise->State()) {
    case v8::Promise::kFulfilled:
      return ResultOf(call, context, promise->Result());
    case v8::Promise::kRejected:
      // The call takes it, so that it is no uncaught error too.
      promise->MarkAsHandled();
      return FailureOf(
          call, DescribeUncaught(isolate_, context, promise->Result(),
                                 kUncaughtInPromise, kMaxEvaluatedTextSize));
    case v8::Promise::kPending:
      waiting_calls_.insert(call);
      break;
  }
  // As V8's inspector waits for a promise that DevTools evaluates.
  std::array<v8::Local<v8::Value>, 2> items = {
      v8::External::New(isolate_, this, v8::kExternalPointerTypeTagDefault),
      v8::BigInt::NewFromUnsigned(isolate_, call)};
  const v8::Local<v8::Array> data =
      v8::Array::New(isolate_, items.data(), items.size());
  const v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Function> on_fulfilled;
  v8::Local<v8::Function> on_rejected;
  // A script can make Then() throw, through the constructor of the promise.
  if (!v8::Function::New(context, &ScriptRunner::OnFulfilled, data)
           .ToLocal(&on_fulfilled) ||
      !v8::Function::New(context, &ScriptRunner::OnRejected, data)
           .ToLocal(&on_rejected) ||
      promise->Then(context, on_fulfilled, on_rejected).IsEmpty()) {
    waiting_calls_.erase(call);
    return FailureOf(call, Failure(isolate_, context, try_catch));
  }
  return std::nullopt;
}

// static
ScriptRunner& ScriptRunner::FromCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    uint64_t& call) {
  const v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  const v8::Local<v8::Array> data = info.Data().As<v8::Array>();
  call = data->Get(context, 1).ToLocalChecked().As<v8::BigInt>()->Uint64Value();
  return *static_cast<ScriptRunner*>(
      data->Get(context, 0)
          .ToLocalChecked()
          .As<v8::External>()
          ->Value(v8::kExternalPointerTypeTagDefault));
}

// static
void ScriptRunner::OnFulfilled(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  uint64_t call = 0;
  ScriptRunner& runner = FromCallback(info, call);
  // A call whose context went has failed already.
  if (runner.waiting_calls_.erase(call) == 0) {
    return;
  }
  runner.settled_.push_back(
      runner.ResultOf(call, info.GetIsolate()->GetCurrentContext(), info[0]));
}

// static
void ScriptRunner::OnRejected(const v8::FunctionCallbackInfo<v8::Value>& info) {
  uint64_t call = 0;
  ScriptRunner& runner = FromCallback(info, call);
  if (runner.waiting_calls_.erase(call) == 0) {
    return;
  }
  v8::Isolate* const isolate = info.GetIsolate();
  runner.settled_.push_back(runner.FailureOf(
      call, DescribeUncaught(isolate, isolate->GetCurrentContext(), info[0],
                             kUncaughtInPromise, kMaxEvaluatedTextSize)));
}

}  // namespace mitto
