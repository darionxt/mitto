#include "mitto/page/page_reporter.h"

#include <stdint.h>

#include <optional>
#include <utility>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/memory_info.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink gives each frame a context group of its own. A Page has one.
constexpr int kContextGroupId = 1;

// The one PageReporter, which V8's callback for rejected promises reaches.
PageReporter* g_reporter = nullptr;

// Returns `view`, which holds Latin-1 or UTF-16, in UTF-8. A lone surrogate
// becomes U+FFFD.
std::string ToUtf8(const v8_inspector::StringView& view) {
  if (view.is8Bit()) {
    // SAFETY: StringView holds length() bytes at characters8().
    const base::span<const uint8_t> latin1 =
        UNSAFE_BUFFERS(base::span(view.characters8(), view.length()));
    return base::UTF16ToUTF8(std::u16string(latin1.begin(), latin1.end()));
  }
  // SAFETY: StringView holds length() code units at characters16().
  const base::span<const uint16_t> utf16 =
      UNSAFE_BUFFERS(base::span(view.characters16(), view.length()));
  return base::UTF16ToUTF8(std::u16string(utf16.begin(), utf16.end()));
}

ConsoleLevel ToConsoleLevel(v8::Isolate::MessageErrorLevel level) {
  switch (level) {
    case v8::Isolate::kMessageDebug:
      return ConsoleLevel::kDebug;
    case v8::Isolate::kMessageInfo:
      return ConsoleLevel::kInfo;
    case v8::Isolate::kMessageWarning:
      return ConsoleLevel::kWarning;
    case v8::Isolate::kMessageError:
      return ConsoleLevel::kError;
    default:
      return ConsoleLevel::kLog;
  }
}

}  // namespace

std::string DescribeUncaught(v8::Isolate* isolate,
                             v8::Local<v8::Context> context,
                             v8::Local<v8::Value> exception,
                             std::string_view prefix,
                             size_t max_size) {
  // What String() throws stays here.
  const v8::TryCatch try_catch(isolate);
  std::optional<std::string> text;
  v8::Local<v8::String> string;
  if (exception->ToString(context).ToLocal(&string)) {
    const v8::String::Utf8Value utf8(isolate, string);
    // SAFETY: Utf8Value holds length() bytes at *utf8.
    text = std::string(UNSAFE_BUFFERS(std::string_view(*utf8, utf8.length())));
  }
  std::string description;
  // What was thrown can be as long as any string.
  base::TruncateUTF8ToByteSize(
      base::StrCat({prefix, text.value_or("exception")}), max_size,
      &description);
  return description;
}

PageReporter::Rejection::Rejection() = default;
PageReporter::Rejection::Rejection(Rejection&&) = default;
PageReporter::Rejection& PageReporter::Rejection::operator=(Rejection&&) =
    default;
PageReporter::Rejection::~Rejection() = default;

PageReporter::PageReporter(v8::Isolate* isolate, SendFrame send)
    : isolate_(isolate),
      send_(std::move(send)),
      inspector_(v8_inspector::V8Inspector::create(isolate, this)) {
  CHECK(!g_reporter);
  g_reporter = this;
  isolate_->SetPromiseRejectCallback(&PageReporter::OnPromiseReject);
  isolate_->AddMessageListenerWithErrorLevel(&PageReporter::OnMessage,
                                             v8::Isolate::kMessageError);
}

PageReporter::~PageReporter() {
  isolate_->RemoveMessageListeners(&PageReporter::OnMessage);
  isolate_->SetPromiseRejectCallback(nullptr);
  g_reporter = nullptr;
}

void PageReporter::ContextCreated(v8::Local<v8::Context> context) {
  context_.Reset(isolate_, context);
  v8_inspector::V8ContextInfo info(context, kContextGroupId,
                                   v8_inspector::StringView());
  // A window's console has memory, as Blink's MainThreadDebugger says.
  info.hasMemoryOnConsole = true;
  inspector_->contextCreated(info);
}

void PageReporter::ContextDestroyed(v8::Local<v8::Context> context) {
  inspector_->contextDestroyed(context);
  // A rejection in that context no longer has a console to go to.
  rejections_.clear();
}

std::vector<PageReporter::Rejection> PageReporter::TakeUnhandledRejections() {
  std::vector<Rejection> rejections;
  rejections.swap(rejections_);
  return rejections;
}

void PageReporter::ReportUnhandledRejections(
    v8::Local<v8::Context> context,
    std::vector<Rejection> rejections) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context);
  for (Rejection& rejection : rejections) {
    // A handler added since then, also by the caller that evaluated it, takes
    // it.
    if (rejection.promise.Get(isolate_)->HasHandler()) {
      continue;
    }
    // Writing it out runs its String(), which may reject more, for next time.
    send_.Run(EncodeUncaughtError(
        DescribeUncaught(isolate_, context, rejection.reason.Get(isolate_),
                         "Uncaught (in promise) ", kMaxPayloadSize)));
  }
}

void PageReporter::ReportException(v8::Local<v8::Value> exception) {
  const v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = isolate_->GetCurrentContext();
  if (context.IsEmpty()) {
    if (context_.IsEmpty()) {
      return;
    }
    context = context_.Get(isolate_);
  }
  const v8::Context::Scope context_scope(context);
  send_.Run(EncodeUncaughtError(DescribeUncaught(
      isolate_, context, exception, "Uncaught ", kMaxPayloadSize)));
}

v8::MaybeLocal<v8::Value> PageReporter::memoryInfo(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context) {
  // MainThreadDebugger::memoryInfo(): console.memory is always bucketized.
  return bindings::ToV8(isolate,
                        MemoryInfo::Create(MemoryInfo::Precision::kBucketized));
}

void PageReporter::consoleAPIMessage(int context_group_id,
                                     v8::Isolate::MessageErrorLevel level,
                                     const v8_inspector::StringView& message,
                                     const v8_inspector::StringView& url,
                                     unsigned line_number,
                                     unsigned column_number,
                                     v8_inspector::V8StackTrace* stack_trace) {
  PageConsoleMessage console;
  console.level = ToConsoleLevel(level);
  base::TruncateUTF8ToByteSize(ToUtf8(message), kMaxConsoleTextSize,
                               &console.text);
  send_.Run(EncodeConsole(console));
}

// static
void PageReporter::OnMessage(v8::Local<v8::Message> message,
                             v8::Local<v8::Value> exception) {
  if (g_reporter) {
    g_reporter->ReportException(exception);
  }
}

// static
void PageReporter::OnPromiseReject(v8::PromiseRejectMessage message) {
  if (!g_reporter || message.GetEvent() != v8::kPromiseRejectWithNoHandler) {
    return;
  }
  v8::Isolate* const isolate = g_reporter->isolate_;
  Rejection rejection;
  rejection.promise.Reset(isolate, message.GetPromise());
  rejection.reason.Reset(isolate, message.GetValue());
  g_reporter->rejections_.push_back(std::move(rejection));
}

}  // namespace mitto
