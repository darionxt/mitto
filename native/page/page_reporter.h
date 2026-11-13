// Tells a Page's Session what the Page's scripts write to the console and
// which errors they leave uncaught, as Blink tells its browser process, once
// the window's error and unhandledrejection listeners have had them
// (error_reporting.h).

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
#include "v8/include/v8-message.h"
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
  // V8 tell this of each promise that is rejected with no handler, and of each
  // error that nothing caught. One at a time.
  PageReporter(v8::Isolate* isolate, SendFrame send);

  PageReporter(const PageReporter&) = delete;
  PageReporter& operator=(const PageReporter&) = delete;

  ~PageReporter() override;

  // Has the console of `context` report here.
  void ContextCreated(v8::Local<v8::Context> context);
  // Says that `context` has gone, as Blink does when a frame's document goes.
  void ContextDestroyed(v8::Local<v8::Context> context);

  // A promise rejected with no handler, and why, as Blink's
  // RejectedPromises::Message holds it.
  struct Rejection {
    Rejection();
    Rejection(Rejection&&);
    Rejection& operator=(Rejection&&);
    ~Rejection();

    v8::Global<v8::Promise> promise;
    v8::Global<v8::Value> reason;
    // Whether it fires events at the window: not for a script whose errors
    // are muted.
    bool fires_events = false;
    // Whether the Session heard of it, once it is reported.
    bool reported_to_session = false;
  };

  // Takes the promises rejected with no handler since the last time, which
  // Blink takes once a microtask checkpoint ends.
  std::vector<Rejection> TakeUnhandledRejections();

  // Reports each of `rejections` that still has no handler, as Blink does from
  // the task that it queues for them: unhandledrejection at the window, and
  // unless a listener canceled it, the Session, as a console says it:
  // "Uncaught (in promise) ...".
  void ReportUnhandledRejections(v8::Local<v8::Context> context,
                                 std::vector<Rejection> rejections);

  // Reports `exception`, which a script threw and nothing caught: the
  // window's error event, and unless a listener canceled it, the Session, as
  // a console says it: "Uncaught TypeError: ...". `message`, V8's message of
  // it, says where it was thrown; without one, V8 makes one.
  void ReportException(v8::Local<v8::Value> exception,
                       v8::Local<v8::Message> message = {});

  // v8_inspector::V8InspectorClient:
  v8::MaybeLocal<v8::Value> memoryInfo(v8::Isolate* isolate,
                                       v8::Local<v8::Context> context) override;
  void consoleAPIMessage(int context_group_id,
                         v8::Isolate::MessageErrorLevel level,
                         const v8_inspector::StringView& message,
                         const v8_inspector::StringView& url,
                         unsigned line_number,
                         unsigned column_number,
                         v8_inspector::V8StackTrace* stack_trace) override;

 private:
  static void OnPromiseReject(v8::PromiseRejectMessage message);
  // V8's message listener, which hears of what a microtask or a script with
  // no handler around it threw.
  static void OnMessage(v8::Local<v8::Message> message,
                        v8::Local<v8::Value> exception);

  // Blink's RejectedPromises::HandlerAdded(): a handler that comes for a
  // promise before its report takes it out, and one that comes after
  // fires rejectionhandled from a later task.
  void HandlerAdded(v8::Local<v8::Promise> promise);
  // Fires rejectionhandled for `rejection`, as Blink's RevokeNow() does.
  void Revoke(Rejection rejection);

  const raw_ptr<v8::Isolate> isolate_;
  const SendFrame send_;
  std::unique_ptr<v8_inspector::V8Inspector> inspector_;
  // The promises rejected with no handler since the last report.
  std::vector<Rejection> rejections_;
  // Those reported with no handler, held weakly, at most the last 1000, as
  // Blink's reported_as_errors_, for a handler that comes later.
  std::vector<Rejection> reported_rejections_;
  // The newest context, for an error reported where none is entered.
  v8::Global<v8::Context> context_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_REPORTER_H_
