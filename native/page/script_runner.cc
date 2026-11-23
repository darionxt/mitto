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
#include "mitto/dom/event_loop.h"
#include "mitto/page/chrome_object.h"
#include "mitto/page/page_frame.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/session_cache_storage.h"
#include "mitto/page/session_cookie_jar.h"
#include "mitto/page/session_indexed_db.h"
#include "mitto/page/session_storage_buckets.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "third_party/icu/source/common/unicode/stringpiece.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "url/gurl.h"
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

// Upstream: services/network/public/cpp/is_potentially_trustworthy.cc
bool IsOriginPotentiallyTrustworthy(const url::Origin& origin) {
  if (origin.opaque()) {
    return false;
  }
  return GURL::SchemeIsCryptographic(origin.scheme()) ||
         IsLocalhost(origin.GetURL()) || origin.scheme() == url::kFileScheme;
}

ScriptRunner::ScriptRunner(v8::Isolate* isolate) : isolate_(isolate) {}

ScriptRunner::~ScriptRunner() {
  if (started_) {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
    isolate_->RemoveMicrotasksCompletedCallback(
        &ScriptRunner::OnMicrotasksCompleted, this);
  }
}

std::optional<std::string> ScriptRunner::Start(
    const PageStart& start,
    scoped_refptr<SessionSender> sender,
    base::RepeatingCallback<void(std::string why)> refuse,
    SessionCookieJar& cookie_jar) {
  sender_ = std::move(sender);
  send_ = base::BindRepeating(&SessionSender::Send, sender_);
  start_ = start;
  cookie_jar_ = &cookie_jar;
  cookie_jar_->SetSend(send_);
  refuse_ = refuse;
  requests_ = std::make_unique<PageRequests>(sender_, std::move(refuse));
  // The top-level frame is the Page's first.
  frame_ = std::make_unique<PageFrame>(*this, /*id=*/1);
  frame_->ExpectOpeningNavigation();
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
  frame_->Start(start_.connection);
  started_ = true;
  base::CurrentThread::Get()->AddTaskObserver(this);
  isolate_->AddMicrotasksCompletedCallback(&ScriptRunner::OnMicrotasksCompleted,
                                           this);
  return std::nullopt;
}

ChromeObjectData& ScriptRunner::NewChromeObjectData(
    base::TimeTicks navigation_start) {
  chrome_object_data_.push_back(
      std::make_unique<ChromeObjectData>(navigation_start));
  return *chrome_object_data_.back();
}

void ScriptRunner::KeepRemovedFrameStorage(
    std::unique_ptr<SessionCacheStorage> cache_storage,
    std::unique_ptr<SessionIndexedDb> indexed_db,
    std::unique_ptr<SessionStorageBuckets> storage_buckets) {
  removed_cache_storage_.push_back(std::move(cache_storage));
  removed_indexed_db_.push_back(std::move(indexed_db));
  removed_storage_buckets_.push_back(std::move(storage_buckets));
}

void ScriptRunner::KeepRemovedFrame(std::unique_ptr<PageFrame> frame) {
  removed_frames_.push_back(std::move(frame));
}

size_t ScriptRunner::FrameCount() const {
  if (!frame_) {
    return 0;
  }
  size_t count = 0;
  std::vector<const Frame*> pending = {frame_.get()};
  while (!pending.empty()) {
    const Frame* const frame = pending.back();
    pending.pop_back();
    ++count;
    for (size_t index = 0; index < frame->ChildCount(); ++index) {
      pending.push_back(frame->ChildAt(index));
    }
  }
  return count;
}

void ScriptRunner::TopLevelContextGoes() {
  for (const uint64_t call : waiting_calls_) {
    settled_.push_back(FailureOf(call, std::string(kContextDestroyed)));
  }
  waiting_calls_.clear();
}

void ScriptRunner::WillProcessTask(const base::PendingTask& pending_task,
                                   bool was_blocked_or_low_priority) {}

void ScriptRunner::DidProcessTask(const base::PendingTask& pending_task) {
  EndTask();
  // No script runs now, so the frames that the task removed go.
  std::vector<std::unique_ptr<PageFrame>> removed;
  removed.swap(removed_frames_);
}

void ScriptRunner::Evaluate(uint64_t call, std::string_view script) {
  // Start() failed, and the Page is about to end.
  if (!started_) {
    send_.Run(EncodeEvaluated(FailureOf(call, "the Page did not start")));
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = frame_->context();
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
  if (!started_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  // As Blink's scheduler does after each task. Its end queues the report of
  // the promises left rejected with no handler, as every checkpoint's does.
  v8::MicrotasksScope::PerformCheckpoint(isolate_);
  QueueRejectionReport();
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

// static
void ScriptRunner::OnMicrotasksCompleted(v8::Isolate* isolate, void* data) {
  static_cast<ScriptRunner*>(data)->QueueRejectionReport();
}

void ScriptRunner::QueueRejectionReport() {
  if (!started_) {
    return;
  }
  // As Blink's RejectedPromises::ProcessQueue(): the promises rejected with no
  // handler since the last checkpoint are reported from a task of their own,
  // so that a handler that a task queued before it adds is in time.
  std::vector<PageReporter::Rejection> rejections =
      reporter_->TakeUnhandledRejections();
  if (rejections.empty()) {
    return;
  }
  ++reports_queued_;
  EventLoop::Get()
      ->TaskRunner(TaskType::kDOMManipulation)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&ScriptRunner::ReportRejections,
                                base::Unretained(this), std::move(rejections)));
}

void ScriptRunner::ReportRejections(
    std::vector<PageReporter::Rejection> rejections) {
  --reports_queued_;
  const v8::HandleScope handle_scope(isolate_);
  reporter_->ReportUnhandledRejections(std::move(rejections));
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

void ScriptRunner::OnRequestBodyCredit(PageCredit credit) {
  RunTask(base::BindOnce(&PageRequests::OnRequestBodyCredit,
                         base::Unretained(requests_.get()), credit));
}

void ScriptRunner::OnIsolation(PageIsolation isolation) {
  RunTask(base::BindOnce(&PageRequests::OnIsolation,
                         base::Unretained(requests_.get()), isolation));
}

void ScriptRunner::RunTask(base::OnceClosure task) {
  // Start() failed, and the Page is about to end.
  if (!started_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  std::move(task).Run();
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
