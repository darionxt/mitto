#include "mitto/page/script_runner.h"

#include <memory>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "third_party/icu/source/common/unicode/stringpiece.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-platform.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-value.h"

namespace mitto {
namespace {

// What v8::JSON::Stringify() hands back when JSON.stringify() writes nothing,
// as for undefined, a function or a symbol. No JSON reads so.
constexpr std::string_view kNothingWritten = "undefined";

// Returns `value` as String() would write it, in UTF-8, or nullopt if that
// throws.
std::optional<std::string> ToUtf8(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context,
                                  v8::Local<v8::Value> value) {
  v8::Local<v8::String> string;
  if (!value->ToString(context).ToLocal(&string)) {
    return std::nullopt;
  }
  const v8::String::Utf8Value text(isolate, string);
  // SAFETY: Utf8Value holds length() bytes at *text.
  return std::string(UNSAFE_BUFFERS(std::string_view(*text, text.length())));
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

std::optional<std::string> ScriptRunner::Start(const PageStart& start) {
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

  const v8::HandleScope handle_scope(isolate_);
  context_.Reset(isolate_, v8::Context::New(isolate_));
  return std::nullopt;
}

PageEvaluated ScriptRunner::Evaluate(uint64_t call, std::string_view script) {
  PageEvaluated evaluated;
  evaluated.call = call;
  // Start() failed, and the Page is about to end.
  if (context_.IsEmpty()) {
    evaluated.failed = true;
    evaluated.text = "the Page did not start";
    return evaluated;
  }
  {
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    const v8::Context::Scope context_scope(context);
    const v8::TryCatch try_catch(isolate_);
    v8::Local<v8::String> source;
    v8::Local<v8::Script> compiled;
    v8::Local<v8::Value> result;
    v8::Local<v8::String> json;
    if (v8::String::NewFromUtf8(isolate_, script.data(),
                                v8::NewStringType::kNormal,
                                base::checked_cast<int>(script.size()))
            .ToLocal(&source) &&
        v8::Script::Compile(context, source).ToLocal(&compiled) &&
        compiled->Run(context).ToLocal(&result) &&
        v8::JSON::Stringify(context, result).ToLocal(&json)) {
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
    } else {
      evaluated.failed = true;
      const std::optional<std::string> exception =
          try_catch.HasCaught()
              ? ToUtf8(isolate_, context, try_catch.Exception())
              : std::nullopt;
      // What was thrown can be as long as any string.
      base::TruncateUTF8ToByteSize(
          "Uncaught " + exception.value_or("exception"), kMaxEvaluatedTextSize,
          &evaluated.text);
    }
  }
  RunPendingTasks();
  return evaluated;
}

void ScriptRunner::RunPendingTasks() {
  while (v8::platform::PumpMessageLoop(&*platform_, isolate_)) {
  }
}

}  // namespace mitto
