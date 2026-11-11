// The Page program, which runs a Page's scripts in a process of its own. The
// library starts it for a Page with --session=, and it serves that Page's
// Session over the channel that the argument names: see session_channel.h and
// docs/runtime.md. Given a script instead, it starts V8, runs the script in a
// new context and prints what the script returns, as String() would write it.
// The context's document is about:blank's, or with --html= the one that the
// HTML parser builds from MARKUP, in UTF-8.
//
// Usage: mitto_page [--enable-blink-features=NAMES]
//                   [--disable-blink-features=NAMES]
//                   [--html=MARKUP] SCRIPT
//        mitto_page [--enable-blink-features=NAMES]
//                   [--disable-blink-features=NAMES] --session=CHANNEL,PARENT
//        mitto_page --version
//
// Each context has the interfaces that the Runtime implements, as the Page's
// runtime-enabled features expose them: the Engine Release's for stable on
// Windows, with those that the options name, separated by commas, turned on or
// off, as Chrome's options of the same names do.
//
// Exits with 1 if the script throws, and with 2 if the arguments are wrong.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/types/expected.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_channel.h"
#include "mitto/page_messages.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-platform.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-snapshot.h"
#include "v8/include/v8-value.h"

// The program holds ICU's whole data file and V8's startup snapshot, which
// page/BUILD.gn writes into it, so that it opens no file to start.
extern "C" {
extern const char mitto_icu_data[];
extern const char mitto_v8_snapshot[];
extern const uint64_t mitto_v8_snapshot_size;
}

namespace {

constexpr int kScriptThrew = 1;
constexpr int kWrongArguments = 2;

constexpr std::string_view kUsage =
    "usage: mitto_page [--enable-blink-features=NAMES] "
    "[--disable-blink-features=NAMES] [--html=MARKUP] SCRIPT | "
    "--session=CHANNEL,PARENT | --version";
constexpr std::string_view kEnableFeatures = "--enable-blink-features=";
constexpr std::string_view kDisableFeatures = "--disable-blink-features=";
constexpr std::string_view kHtml = "--html=";

void WriteLine(FILE* stream, std::string_view text) {
  // SAFETY: fwrite() reads text.size() bytes from text.data(), which `text`
  // holds.
  UNSAFE_BUFFERS(fwrite(text.data(), 1, text.size(), stream));
  fputc('\n', stream);
}

// Returns `value` as String() would write it, in UTF-8, or nullopt if that
// throws, which leaves the exception to the caller's TryCatch.
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

// Runs `source` in a new context whose document `html` builds, and writes what
// it returns to standard output, or what it throws to standard error. Returns
// whether it finished without throwing.
bool RunScript(v8::Isolate* isolate,
               std::string_view html,
               std::string_view source) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context =
      mitto::NewPageContext(isolate, base::UTF8ToUTF16(html));
  const v8::Context::Scope context_scope(context);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> source_string;
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (v8::String::NewFromUtf8(isolate, source.data(),
                              v8::NewStringType::kNormal,
                              base::checked_cast<int>(source.size()))
          .ToLocal(&source_string) &&
      v8::Script::Compile(context, source_string).ToLocal(&script) &&
      script->Run(context).ToLocal(&result)) {
    if (const std::optional<std::string> text =
            ToUtf8(isolate, context, result)) {
      WriteLine(stdout, *text);
      return true;
    }
  }
  const std::optional<std::string> exception =
      try_catch.HasCaught() ? ToUtf8(isolate, context, try_catch.Exception())
                            : std::nullopt;
  WriteLine(stderr, "Uncaught " + exception.value_or("exception"));
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // SAFETY: main() gets `argc` arguments in `argv`.
  const base::span<char*> arguments =
      UNSAFE_BUFFERS(base::span(argv, base::checked_cast<size_t>(argc)));
  if (arguments.size() == 2 && std::string_view(arguments[1]) == "--version") {
    WriteLine(stdout, v8::V8::GetVersion());
    return 0;
  }
  if (arguments.size() < 2) {
    WriteLine(stderr, kUsage);
    return kWrongArguments;
  }
  // Chrome's options: a second one of the same name replaces the first.
  std::string_view enable;
  std::string_view disable;
  std::optional<std::string_view> html;
  for (const std::string_view option :
       arguments.subspan(1u).first(arguments.size() - 2)) {
    if (option.starts_with(kEnableFeatures)) {
      enable = option.substr(kEnableFeatures.size());
    } else if (option.starts_with(kDisableFeatures)) {
      disable = option.substr(kDisableFeatures.size());
    } else if (option.starts_with(kHtml)) {
      html = option.substr(kHtml.size());
    } else {
      WriteLine(stderr, kUsage);
      return kWrongArguments;
    }
  }
  base::expected<mitto::bindings::RuntimeFeatureSet, std::string> features =
      mitto::bindings::RuntimeFeatureSet::FromSwitches(enable, disable);
  if (!features.has_value()) {
    WriteLine(stderr, "mitto_page: no runtime-enabled feature is named " +
                          features.error());
    return kWrongArguments;
  }
  const std::string_view argument = arguments.back();
  std::optional<mitto::SessionArgument> session;
  if (argument.starts_with(mitto::kSessionArgumentPrefix)) {
    session = mitto::ParseSessionArgument(argument);
    if (!session) {
      WriteLine(stderr, "--session= names no channel and process");
      return kWrongArguments;
    }
    if (html) {
      // A Session's Page gets its document from the Session.
      WriteLine(stderr, kUsage);
      return kWrongArguments;
    }
  }

  // ICU reads its data from memory, as in Chrome's renderers, which map the
  // file that the browser gives them.
  UErrorCode status = U_ZERO_ERROR;
  udata_setCommonData(mitto_icu_data, &status);
  CHECK(U_SUCCESS(status));

  // Before anything of V8 asks for a random number, as in Chrome's renderers.
  v8::V8::SetEntropySource(&mitto::GenerateEntropy);
  v8::StartupData snapshot = {mitto_v8_snapshot,
                              base::checked_cast<int>(mitto_v8_snapshot_size)};
  v8::V8::SetSnapshotDataBlob(&snapshot);
  const std::unique_ptr<v8::Platform> platform =
      v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  CHECK(v8::V8::Initialize());

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared =
      std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  // The heap of the Runtime's C++ objects, which V8 collects with its own, as
  // Blink's renderers have it. The isolate owns it.
  params.cpp_heap =
      v8::CppHeap::Create(platform.get(), v8::CppHeapCreateParams({}))
          .release();
  v8::Isolate* const isolate = v8::Isolate::New(params);
  bool finished = false;
  {
    const v8::Isolate::Scope isolate_scope(isolate);
    const std::unique_ptr<mitto::bindings::PerIsolateData> data =
        mitto::bindings::PerIsolateData::Create(isolate, std::move(*features));
    if (session) {
      mitto::ServeSession(*platform, isolate, *std::move(session));
    } else {
      finished = RunScript(isolate, html.value_or(""), argument);
    }
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return finished ? 0 : kScriptThrew;
}
