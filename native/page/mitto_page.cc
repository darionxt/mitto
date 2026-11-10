// The Page program, which runs a Page's scripts in a process of its own. For
// now it starts V8, runs the script on its command line in a new context and
// prints what the script returns, as String() would write it. It talks to
// nothing else yet.
//
// Usage: mitto_page SCRIPT
//        mitto_page --version
//
// Exits with 1 if the script throws, and with 2 if the arguments are wrong.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
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

// Runs `source` in a new context, and writes what it returns to standard
// output, or what it throws to standard error. Returns whether it finished
// without throwing.
bool RunScript(v8::Isolate* isolate, std::string_view source) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = v8::Context::New(isolate);
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
  if (arguments.size() != 2) {
    WriteLine(stderr, "usage: mitto_page SCRIPT | --version");
    return kWrongArguments;
  }
  const std::string_view argument = arguments[1];
  if (argument == "--version") {
    WriteLine(stdout, v8::V8::GetVersion());
    return 0;
  }

  // ICU reads its data from memory, as in Chrome's renderers, which map the
  // file that the browser gives them.
  UErrorCode status = U_ZERO_ERROR;
  udata_setCommonData(mitto_icu_data, &status);
  CHECK(U_SUCCESS(status));

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
  v8::Isolate* const isolate = v8::Isolate::New(params);
  bool finished = false;
  {
    const v8::Isolate::Scope isolate_scope(isolate);
    finished = RunScript(isolate, argument);
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return finished ? 0 : kScriptThrew;
}
