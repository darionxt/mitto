// The Page program, which runs a Page's scripts in a process of its own. The
// library starts it for a Page with --session=, and it serves that Page's
// Session over the channel that the argument names: see session_channel.h and
// docs/runtime.md. Given a script instead, it starts V8, runs the script in a
// new context, runs the event loop until no task or timer is left, and prints
// what the script returns, as String() would write it, or what the promise it
// returns was fulfilled with.
// The context's document is about:blank's, or with --html= the one that the
// HTML parser builds from MARKUP. SCRIPT "-" reads the script from standard
// input, for one longer than a command line holds. Both are in UTF-8.
//
// Usage: mitto_page [--enable-blink-features=NAMES]
//                   [--disable-blink-features=NAMES] [--js-flags=FLAGS]
//                   [--html=MARKUP] SCRIPT
//        mitto_page [--enable-blink-features=NAMES]
//                   [--disable-blink-features=NAMES] [--js-flags=FLAGS]
//                   --session=CHANNEL,PARENT
//        mitto_page --version
//
// Each context has the interfaces that the Runtime implements, as the Page's
// runtime-enabled features expose them: the Engine Release's for stable on
// Windows, with those that the options name, separated by commas, turned on or
// off, as Chrome's options of the same names do, and window.chrome and the
// rest of what Chrome's renderer adds. A Session's Page takes no such options:
// its features and V8's flags come in its start message, so V8 starts once
// that has come. V8 has the flags of the Engine Release's renderer, then the
// Persona's, then those of FLAGS, separated by commas, as Chrome's --js-flags
// gives them.
//
// Exits with 1 if the script throws or its promise is rejected or never
// settles, and with 2 if the arguments are wrong. What a timer or a microtask
// throws goes to standard error, as a console would show it.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/at_exit.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_observer.h"
#include "base/types/expected.h"
#include "build/build_config.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/dom/idle_task_controller.h"
#include "mitto/dom/window.h"
#include "mitto/page/chrome_object.h"
#include "mitto/page/page_scheduler.h"
#include "mitto/page/page_isolate.h"
#include "mitto/page/page_platform.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_channel.h"
#include "mitto/page/v8_flags.h"
#include "mitto/page_messages.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-inspector.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-platform.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-snapshot.h"
#include "v8/include/v8-value.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#endif

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
    "[--disable-blink-features=NAMES] [--js-flags=FLAGS] [--html=MARKUP] "
    "SCRIPT | --session=CHANNEL,PARENT | --version";
constexpr std::string_view kEnableFeatures = "--enable-blink-features=";
constexpr std::string_view kDisableFeatures = "--disable-blink-features=";
constexpr std::string_view kJsFlags = "--js-flags=";
// The memory in GiB that the command line's V8 works out its heap's limits
// from: a Machine's of the most common kind (docs/persona.md). A Session's Page
// takes its Machine's from its start.
constexpr int kMemoryGib = 8;
constexpr std::string_view kHtml = "--html=";
constexpr std::string_view kStandardInput = "-";

// The program's arguments, in UTF-8. Windows gives main() its arguments in the
// system's ANSI code page, which loses each character that it cannot write,
// so there they come from the command line in UTF-16, as base::CommandLine
// takes them.
std::vector<std::string> ArgumentsInUtf8(int argc, char** argv) {
#if BUILDFLAG(IS_WIN)
  int count = 0;
  wchar_t** const wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  CHECK(wide);
  // SAFETY: CommandLineToArgvW() gives `count` arguments.
  const base::span<wchar_t*> wide_arguments =
      UNSAFE_BUFFERS(base::span(wide, base::checked_cast<size_t>(count)));
  std::vector<std::string> arguments;
  for (const wchar_t* const argument : wide_arguments) {
    arguments.push_back(base::WideToUTF8(argument));
  }
  ::LocalFree(wide);
  return arguments;
#else
  // SAFETY: main() gets `argc` arguments in `argv`.
  const base::span<char*> arguments =
      UNSAFE_BUFFERS(base::span(argv, base::checked_cast<size_t>(argc)));
  return std::vector<std::string>(arguments.begin(), arguments.end());
#endif
}

// What standard input holds, byte for byte, or nullopt if it cannot be read.
std::optional<std::string> ReadStandardInput() {
#if BUILDFLAG(IS_WIN)
  // Not as text, which would end at a Ctrl-Z and turn CR LF into LF.
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  std::string text;
  if (!base::ReadStreamToString(stdin, &text)) {
    return std::nullopt;
  }
  return text;
}

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

// Writes what a timer or a microtask threw, which nothing caught, as a console
// shows it.
void PrintUncaught(v8::Local<v8::Message> message,
                   v8::Local<v8::Value> exception) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::TryCatch try_catch(isolate);
  const std::optional<std::string> text =
      ToUtf8(isolate, isolate->GetCurrentContext(), exception);
  WriteLine(stderr, "Uncaught " + text.value_or("exception"));
}

// The command line's client of V8's inspector, which a Page's console has as
// in Chrome: it gives console what the inspector adds, such as createTask()
// and memory, and drops what the console writes, as the command line always
// has.
class CommandLineInspector final : public v8_inspector::V8InspectorClient {
 public:
  explicit CommandLineInspector(v8::Isolate* isolate)
      : inspector_(v8_inspector::V8Inspector::create(isolate, this)) {}

  CommandLineInspector(const CommandLineInspector&) = delete;
  CommandLineInspector& operator=(const CommandLineInspector&) = delete;

  ~CommandLineInspector() override = default;

  void ContextCreated(v8::Local<v8::Context> context) {
    v8_inspector::V8ContextInfo info(context, /*contextGroupId=*/1,
                                     v8_inspector::StringView());
    info.hasMemoryOnConsole = true;
    inspector_->contextCreated(info);
  }

  // v8_inspector::V8InspectorClient:
  v8::MaybeLocal<v8::Value> memoryInfo(
      v8::Isolate* isolate,
      v8::Local<v8::Context> context) override {
    return mitto::bindings::ToV8(
        isolate,
        mitto::MemoryInfo::Create(mitto::MemoryInfo::Precision::kBucketized));
  }

 private:
  std::unique_ptr<v8_inspector::V8Inspector> inspector_;
};

// V8 as the Page program runs it: its platform, one isolate, which the main
// thread has entered, and the bindings' data of that isolate.
struct PageV8 {
  std::unique_ptr<mitto::PagePlatform> platform;
  raw_ptr<v8::Isolate> isolate = nullptr;
  std::unique_ptr<mitto::bindings::PerIsolateData> data;
};

// Starts V8 for a machine with `memory_gib` of memory, with the flags of the
// Engine Release's renderer, then `v8_flags`, each as gin writes one, such as
// "--no-foo", then `js_flags`, set before V8 initialises, as gin sets Chrome's,
// and makes its isolate as Blink makes a main thread's, with `features`, on the
// main thread, whose task runner is `main_thread`.
void StartV8(PageV8& v8,
             scoped_refptr<base::SingleThreadTaskRunner> main_thread,
             int memory_gib,
             base::span<const std::string> v8_flags,
             base::span<const std::string> js_flags,
             mitto::bindings::RuntimeFeatureSet features) {
  // The flags of the Page's start come after those that every Page of the
  // Engine Release has, so that they override them, and --js-flags= last.
  mitto::SetEngineV8Flags(memory_gib);
  mitto::SetV8Flags(v8_flags);
  mitto::SetV8Flags(js_flags);
  // Before anything of V8 asks for a random number, as in Chrome's renderers.
  v8::V8::SetEntropySource(&mitto::GenerateEntropy);
  v8::StartupData snapshot = {mitto_v8_snapshot,
                              base::checked_cast<int>(mitto_v8_snapshot_size)};
  v8::V8::SetSnapshotDataBlob(&snapshot);
  v8.platform = std::make_unique<mitto::PagePlatform>(
      v8::platform::NewDefaultPlatform(), std::move(main_thread));
  v8::V8::InitializePlatform(v8.platform.get());
  CHECK(v8::V8::Initialize());

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared =
      std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  // The heap of the Runtime's C++ objects, which V8 collects with its own, as
  // Blink's renderers have it. The isolate owns it.
  params.cpp_heap =
      v8::CppHeap::Create(v8.platform.get(), v8::CppHeapCreateParams({}))
          .release();
  mitto::ConfigurePageIsolate(params, memory_gib);
  v8.isolate = v8::Isolate::New(params);
  // Microtasks run when the outermost script that the Runtime runs returns,
  // and after each task, as Blink has them.
  v8.isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kScoped);
  mitto::InitializePageIsolate(v8.isolate);
  v8.isolate->Enter();
  v8.data =
      mitto::bindings::PerIsolateData::Create(v8.isolate, std::move(features));
}

// Ends what StartV8() started, if it did. V8's tasks that wait on the main
// thread go while the isolate still lives.
void StopV8(PageV8& v8, std::unique_ptr<mitto::PageScheduler> main_thread) {
  if (!v8.isolate) {
    return;
  }
  v8.data.reset();
  v8.isolate->Exit();
  main_thread.reset();
  v8::Isolate* const isolate = v8.isolate;
  v8.isolate = nullptr;
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
}

// Starts V8 for a Session's Page with the memory, the features and the V8 flags
// that its start says, then `js_flags`. Returns the isolate, or why the Page
// refuses the start.
base::expected<v8::Isolate*, std::string> StartV8ForSession(
    PageV8* v8,
    scoped_refptr<base::SingleThreadTaskRunner> main_thread,
    std::vector<std::string> js_flags,
    const mitto::PageStart& start) {
  base::expected<mitto::bindings::RuntimeFeatureSet, std::string> features =
      mitto::bindings::RuntimeFeatureSet::FromSwitches(
          base::JoinString(start.features.enable_blink_features, ","),
          base::JoinString(start.features.disable_blink_features, ","));
  if (!features.has_value()) {
    return base::unexpected("no runtime-enabled feature is named " +
                            features.error());
  }
  StartV8(*v8, std::move(main_thread), start.memory, start.features.v8_flags,
          js_flags, *std::move(features));
  return v8->isolate.get();
}

// The event loop of the command line: after each task, a microtask
// checkpoint, as in a Page, and the end of the loop once no task is ready and
// no timer is set.
class CommandLineLoop final : public base::TaskObserver {
 public:
  CommandLineLoop(v8::Isolate* isolate, base::RunLoop& run_loop)
      : isolate_(isolate), run_loop_(run_loop) {
    base::CurrentThread::Get()->AddTaskObserver(this);
  }

  CommandLineLoop(const CommandLineLoop&) = delete;
  CommandLineLoop& operator=(const CommandLineLoop&) = delete;

  ~CommandLineLoop() override {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
  }

  // base::TaskObserver:
  void WillProcessTask(const base::PendingTask& pending_task,
                       bool was_blocked_or_low_priority) override {}
  void DidProcessTask(const base::PendingTask& pending_task) override {
    const v8::HandleScope handle_scope(isolate_);
    v8::MicrotasksScope::PerformCheckpoint(isolate_);
    if (mitto::Window::Current().TimerCount() == 0 &&
        mitto::Window::Current().IdleCallbackCount() == 0) {
      run_loop_->QuitWhenIdle();
    }
  }

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const raw_ref<base::RunLoop> run_loop_;
};

// Runs `source` in a new context whose document `html` builds, then the event
// loop until nothing is left, and writes what the script returns to standard
// output, or what it threw to standard error. Returns whether it finished
// without throwing.
bool RunScript(v8::Isolate* isolate,
               std::string_view html,
               std::string_view source) {
  const v8::HandleScope handle_scope(isolate);
  // Its about:blank's loading started when the context did.
  mitto::ChromeObjectData chrome_data(
      mitto::bindings::PerIsolateData::From(isolate)->time_origin());
  CommandLineInspector inspector(isolate);
  const v8::Local<v8::Context> context = mitto::NewPageContext(
      isolate, base::UTF8ToUTF16(html),
      [&inspector, &chrome_data](v8::Local<v8::Context> context) {
        mitto::InitializePageContext(context);
        inspector.ContextCreated(context);
        mitto::InstallChromeObject(context, chrome_data);
      });
  const v8::Context::Scope context_scope(context);
  mitto::DOMTimerCoordinator timers(isolate, context);
  mitto::Window::Current().SetTimers(&timers);
  mitto::IdleTaskController idle_tasks(isolate, context);
  mitto::Window::Current().SetIdleTasks(&idle_tasks);
  mitto::Window::Current().SetDocumentLoadTiming(chrome_data.load_timing.Get());
  isolate->AddMessageListenerWithErrorLevel(&PrintUncaught,
                                            v8::Isolate::kMessageError);

  v8::Global<v8::Value> result;
  v8::Global<v8::Value> exception;
  bool threw = false;
  base::RunLoop run_loop;
  const CommandLineLoop loop(isolate, run_loop);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](v8::Isolate* isolate, std::string_view source,
             v8::Global<v8::Value>* result, v8::Global<v8::Value>* exception,
             bool* threw) {
            const v8::HandleScope handle_scope(isolate);
            const v8::Local<v8::Context> context = isolate->GetCurrentContext();
            const v8::MicrotasksScope microtasks_scope(
                context, v8::MicrotasksScope::kRunMicrotasks);
            const v8::TryCatch try_catch(isolate);
            v8::Local<v8::String> source_string;
            v8::Local<v8::Script> script;
            v8::Local<v8::Value> value;
            if (v8::String::NewFromUtf8(isolate, source.data(),
                                        v8::NewStringType::kNormal,
                                        base::checked_cast<int>(source.size()))
                    .ToLocal(&source_string) &&
                v8::Script::Compile(context, source_string).ToLocal(&script) &&
                script->Run(context).ToLocal(&value)) {
              result->Reset(isolate, value);
              return;
            }
            *threw = true;
            if (try_catch.HasCaught()) {
              exception->Reset(isolate, try_catch.Exception());
            }
          },
          isolate, source, &result, &exception, &threw));
  run_loop.Run();
  mitto::Window::Current().SetTimers(nullptr);
  mitto::Window::Current().SetIdleTasks(nullptr);
  chrome_data.detached = true;

  const v8::TryCatch try_catch(isolate);
  if (threw) {
    const std::optional<std::string> text =
        exception.IsEmpty() ? std::nullopt
                            : ToUtf8(isolate, context, exception.Get(isolate));
    WriteLine(stderr, "Uncaught " + text.value_or("exception"));
    return false;
  }
  v8::Local<v8::Value> value = result.Get(isolate);
  if (value->IsPromise()) {
    const v8::Local<v8::Promise> promise = value.As<v8::Promise>();
    switch (promise->State()) {
      case v8::Promise::kFulfilled:
        value = promise->Result();
        break;
      case v8::Promise::kRejected: {
        const std::optional<std::string> text =
            ToUtf8(isolate, context, promise->Result());
        WriteLine(stderr,
                  "Uncaught (in promise) " + text.value_or("exception"));
        return false;
      }
      case v8::Promise::kPending:
        WriteLine(stderr, "the promise did not settle");
        return false;
    }
  }
  if (const std::optional<std::string> text = ToUtf8(isolate, context, value)) {
    WriteLine(stdout, *text);
    return true;
  }
  // What String() threw for it.
  const std::optional<std::string> thrown =
      try_catch.HasCaught() ? ToUtf8(isolate, context, try_catch.Exception())
                            : std::nullopt;
  WriteLine(stderr, "Uncaught " + thrown.value_or("exception"));
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> arguments = ArgumentsInUtf8(argc, argv);
  if (arguments.size() == 2 && arguments[1] == "--version") {
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
  std::string_view js_flags;
  bool enable_given = false;
  bool disable_given = false;
  std::optional<std::string_view> html;
  for (const std::string_view option :
       base::span(arguments).subspan(1u).first(arguments.size() - 2)) {
    if (option.starts_with(kEnableFeatures)) {
      enable = option.substr(kEnableFeatures.size());
      enable_given = true;
    } else if (option.starts_with(kDisableFeatures)) {
      disable = option.substr(kDisableFeatures.size());
      disable_given = true;
    } else if (option.starts_with(kJsFlags)) {
      js_flags = option.substr(kJsFlags.size());
    } else if (option.starts_with(kHtml)) {
      html = option.substr(kHtml.size());
    } else {
      WriteLine(stderr, kUsage);
      return kWrongArguments;
    }
  }
  std::string_view argument = arguments.back();
  std::optional<mitto::SessionArgument> session;
  if (argument.starts_with(mitto::kSessionArgumentPrefix)) {
    session = mitto::ParseSessionArgument(argument);
    if (!session) {
      WriteLine(stderr, "--session= names no channel and process");
      return kWrongArguments;
    }
    if (html || enable_given || disable_given) {
      // A Session's Page gets its document from the Session, and its features
      // in its start message.
      WriteLine(stderr, kUsage);
      return kWrongArguments;
    }
  }
  std::optional<mitto::bindings::RuntimeFeatureSet> features;
  if (!session) {
    base::expected<mitto::bindings::RuntimeFeatureSet, std::string> set =
        mitto::bindings::RuntimeFeatureSet::FromSwitches(enable, disable);
    if (!set.has_value()) {
      WriteLine(stderr, "mitto_page: no runtime-enabled feature is named " +
                            set.error());
      return kWrongArguments;
    }
    features = *std::move(set);
  }
  std::optional<std::string> script_from_input;
  if (argument == kStandardInput) {
    script_from_input = ReadStandardInput();
    if (!script_from_input) {
      WriteLine(stderr,
                "mitto_page: cannot read the script from standard input");
      return kWrongArguments;
    }
    argument = *script_from_input;
  }

  const base::AtExitManager at_exit_manager;
  // What //base reads of it is not on this command line.
  base::CommandLine::Init(0, nullptr);
  // Each of Chrome's processes sets its base::Features, which also makes
  // base::RandBytes() take BoringSSL's bytes, as crypto.getRandomValues()
  // does, where macOS's getentropy() gives at most 256 at a time. A Page has
  // each one's default.
  base::FeatureList::SetInstance(std::make_unique<base::FeatureList>());

  // ICU reads its data from memory, as in Chrome's renderers, which map the
  // file that the browser gives them.
  UErrorCode status = U_ZERO_ERROR;
  udata_setCommonData(mitto_icu_data, &status);
  CHECK(U_SUCCESS(status));

  // The main thread's scheduler is the Page's event loop, which V8's own tasks
  // for the main thread join, as Chrome's renderers have it.
  auto main_thread = std::make_unique<mitto::PageScheduler>();

  PageV8 v8;
  bool finished = false;
  if (session) {
    mitto::ServeSession(
        *std::move(session),
        base::BindOnce(&StartV8ForSession, base::Unretained(&v8),
                       main_thread->task_runner(),
                       mitto::SplitJsFlags(js_flags)));
  } else {
    StartV8(v8, main_thread->task_runner(), kMemoryGib, {},
            mitto::SplitJsFlags(js_flags), *std::move(features));
    finished = RunScript(v8.isolate, html.value_or(""), argument);
  }
  StopV8(v8, std::move(main_thread));
  return finished ? 0 : kScriptThrew;
}
