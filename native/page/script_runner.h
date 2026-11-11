// Runs a Page's scripts on V8, in one context, on the Page program's main
// thread, and sends what each returns as JSON, with what they write to the
// console and the errors that they leave uncaught.

#ifndef MITTO_PAGE_SCRIPT_RUNNER_H_
#define MITTO_PAGE_SCRIPT_RUNNER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-value.h"

namespace v8 {
class Isolate;
class Platform;
}  // namespace v8

namespace mitto {

// Fills `buffer` from the system's random source, which needs no file, for
// V8's random numbers, as Chrome's renderers do.
bool GenerateEntropy(unsigned char* buffer, size_t size);

class ScriptRunner {
 public:
  ScriptRunner(v8::Platform& platform, v8::Isolate* isolate);

  ScriptRunner(const ScriptRunner&) = delete;
  ScriptRunner& operator=(const ScriptRunner&) = delete;

  ~ScriptRunner();

  // Sets the time zone and the locale that ICU, and so V8, report, so that
  // neither is read from the machine, and makes the context. Returns why not,
  // if ICU does not know one of them. What the Page sends from then on goes
  // through `send`.
  std::optional<std::string> Start(const PageStart& start, SendFrame send);

  // Runs `script` in the context that Start() made, and sends its result as
  // JSON, or why it has none. When the result is a promise, that waits until
  // the promise settles, which another script may do, and later scripts run
  // meanwhile.
  void Evaluate(uint64_t call, std::string_view script);

 private:
  // Returns `value`, the result of `call`, as JSON, or why it cannot be.
  PageEvaluated ResultOf(uint64_t call,
                         v8::Local<v8::Context> context,
                         v8::Local<v8::Value> value);

  // Returns that `call` failed, and why.
  static PageEvaluated FailureOf(uint64_t call, std::string text);

  // Returns the result of `call` if `promise` has settled. If not, returns
  // nullopt and queues the result in `settled_` once it settles.
  std::optional<PageEvaluated> SettledResultOf(uint64_t call,
                                               v8::Local<v8::Context> context,
                                               v8::Local<v8::Promise> promise);

  // Returns the runner, and stores the call, that SettledResultOf() gave a
  // callback of the promise.
  static ScriptRunner& FromCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info,
      uint64_t& call);
  static void OnFulfilled(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnRejected(const v8::FunctionCallbackInfo<v8::Value>& info);

  // Runs what V8 has posted for the main thread, such as its garbage
  // collector's tasks.
  void RunPendingTasks();

  const raw_ref<v8::Platform> platform_;
  const raw_ptr<v8::Isolate> isolate_;
  SendFrame send_;
  std::unique_ptr<PageReporter> reporter_;
  // The results of the calls that the current task has settled, which go to
  // the Session once the task's uncaught errors have.
  std::vector<PageEvaluated> settled_;
  v8::Global<v8::Context> context_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SCRIPT_RUNNER_H_
