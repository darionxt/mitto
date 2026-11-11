#include "mitto/page/script_runner.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "mitto/dom/page_interfaces.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "third_party/icu/source/common/unicode/stringpiece.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "v8/include/libplatform/libplatform.h"
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

ScriptRunner::ScriptRunner(v8::Platform& platform, v8::Isolate* isolate)
    : platform_(platform), isolate_(isolate) {}

ScriptRunner::~ScriptRunner() = default;

std::optional<std::string> ScriptRunner::Start(const PageStart& start,
                                               SendFrame send) {
  send_ = std::move(send);
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
  isolate_->DateTimeConfigurationChangeNotification(
      v8::Isolate::TimeZoneDetection::kSkip);
  isolate_->LocaleConfigurationChangeNotification();
  reporter_ = std::make_unique<PageReporter>(isolate_, send_);

  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = v8::Context::New(isolate_);
  {
    const v8::Context::Scope context_scope(context);
    InstallPageInterfaces(context);
  }
  context_.Reset(isolate_, context);
  // Once the context has its interfaces, as Blink tells its inspector.
  reporter_->ContextCreated(context);
  return std::nullopt;
}

void ScriptRunner::Evaluate(uint64_t call, std::string_view script) {
  // Start() failed, and the Page is about to end.
  if (context_.IsEmpty()) {
    send_.Run(EncodeEvaluated(FailureOf(call, "the Page did not start")));
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  std::optional<PageEvaluated> evaluated;
  {
    const v8::Context::Scope context_scope(context);
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::String> source;
    v8::Local<v8::Script> compiled;
    v8::Local<v8::Value> result;
    if (!v8::String::NewFromUtf8(isolate_, script.data(),
                                 v8::NewStringType::kNormal,
                                 base::checked_cast<int>(script.size()))
             .ToLocal(&source) ||
        !v8::Script::Compile(context, source).ToLocal(&compiled) ||
        !compiled->Run(context).ToLocal(&result)) {
      evaluated = FailureOf(call, Failure(isolate_, context, try_catch));
    } else if (result->IsPromise()) {
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
  RunPendingTasks();
  // As Blink does after each task.
  isolate_->PerformMicrotaskCheckpoint();
  reporter_->ReportUnhandledRejections(context);
  // After the errors that the task left uncaught, so that the Session has
  // heard of them by the time it has a result. Writing them out may have run
  // more microtasks, which may have settled more.
  std::vector<PageEvaluated> settled;
  settled.swap(settled_);
  for (const PageEvaluated& result : settled) {
    send_.Run(EncodeEvaluated(result));
  }
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
  runner.settled_.push_back(
      runner.ResultOf(call, info.GetIsolate()->GetCurrentContext(), info[0]));
}

// static
void ScriptRunner::OnRejected(const v8::FunctionCallbackInfo<v8::Value>& info) {
  uint64_t call = 0;
  ScriptRunner& runner = FromCallback(info, call);
  v8::Isolate* const isolate = info.GetIsolate();
  runner.settled_.push_back(runner.FailureOf(
      call, DescribeUncaught(isolate, isolate->GetCurrentContext(), info[0],
                             kUncaughtInPromise, kMaxEvaluatedTextSize)));
}

void ScriptRunner::RunPendingTasks() {
  while (v8::platform::PumpMessageLoop(&*platform_, isolate_)) {
  }
}

}  // namespace mitto
