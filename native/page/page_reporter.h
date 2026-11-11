// Tells a Page's Session what the Page's scripts write to the console and
// which errors they leave uncaught, as Blink tells its browser process.

#ifndef MITTO_PAGE_PAGE_REPORTER_H_
#define MITTO_PAGE_PAGE_REPORTER_H_

#include <stddef.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-inspector.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-value.h"

namespace mitto {

// Sends a frame to the Session. Called on the Page program's main thread.
using SendFrame = base::RepeatingCallback<void(std::string frame)>;

// Returns `prefix` and `exception` as String() would write it, in UTF-8 and cut
// to at most `max_size` bytes, as a console shows an error that nothing
// caught: "Uncaught TypeError: x is not a function". If String() throws, it
// says "exception" instead.
std::string DescribeUncaught(v8::Isolate* isolate,
                             v8::Local<v8::Context> context,
                             v8::Local<v8::Value> exception,
                             std::string_view prefix,
                             size_t max_size);

// The console of a Page is V8's own, as in Chrome, where V8 installs it and
// Blink hears of each call through V8's inspector. So this is the client of an
// inspector, which V8 hands each console message, written as Chrome's log
// writes it.
class PageReporter final : public v8_inspector::V8InspectorClient {
 public:
  // Starts an inspector on `isolate`, as Blink does for each isolate, and has
  // V8 tell this of each promise that is rejected with no handler. One at a
  // time.
  PageReporter(v8::Isolate* isolate, SendFrame send);

  PageReporter(const PageReporter&) = delete;
  PageReporter& operator=(const PageReporter&) = delete;

  ~PageReporter() override;

  // Has the console of `context` report here.
  void ContextCreated(v8::Local<v8::Context> context);

  // Reports each promise that is still rejected with no handler, as Blink does
  // once a task and its microtasks have run.
  void ReportUnhandledRejections(v8::Local<v8::Context> context);

  // v8_inspector::V8InspectorClient:
  void consoleAPIMessage(int context_group_id,
                         v8::Isolate::MessageErrorLevel level,
                         const v8_inspector::StringView& message,
                         const v8_inspector::StringView& url,
                         unsigned line_number,
                         unsigned column_number,
                         v8_inspector::V8StackTrace* stack_trace) override;

 private:
  struct Rejection {
    Rejection();
    Rejection(Rejection&&);
    Rejection& operator=(Rejection&&);
    ~Rejection();

    v8::Global<v8::Promise> promise;
    v8::Global<v8::Value> reason;
  };

  static void OnPromiseReject(v8::PromiseRejectMessage message);

  const raw_ptr<v8::Isolate> isolate_;
  const SendFrame send_;
  std::unique_ptr<v8_inspector::V8Inspector> inspector_;
  // The promises rejected with no handler since the last report.
  std::vector<Rejection> rejections_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_REPORTER_H_
