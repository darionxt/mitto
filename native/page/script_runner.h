// Runs a Page's scripts on V8, in one context, on the Page program's main
// thread, and gives back what each returns as JSON.

#ifndef MITTO_PAGE_SCRIPT_RUNNER_H_
#define MITTO_PAGE_SCRIPT_RUNNER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-persistent-handle.h"

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
  // if ICU does not know one of them.
  std::optional<std::string> Start(const PageStart& start);

  // Runs `script` in the context that Start() made, and returns its result as
  // JSON, or why it has none.
  PageEvaluated Evaluate(uint64_t call, std::string_view script);

 private:
  // Runs what V8 has posted for the main thread, such as its garbage
  // collector's tasks.
  void RunPendingTasks();

  const raw_ref<v8::Platform> platform_;
  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SCRIPT_RUNNER_H_
