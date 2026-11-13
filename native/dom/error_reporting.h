// What a window does with an error that a script left uncaught, and with a
// promise that it rejected with no handler, before the console hears of it,
// as Blink's V8Initializer, ExecutionContext and RejectedPromises do: it fires
// error, or unhandledrejection, at the window, and only what no listener
// canceled goes on to the console, which for a Page is the Session.
//
// An error of a script whose errors are muted, one of another origin fetched
// without CORS, reaches the window's listeners as "Script error.", with no
// place and a null error, and its rejections fire no event; the console still
// hears all of it. V8 says which script an error belongs to, and whether its
// errors are muted, through the ScriptOrigin that the script was compiled
// with: shared cross-origin for a script whose errors are not muted.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_initializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/capture_source_location.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/rejected_promises.cc
// Upstream: third_party/blink/renderer/core/execution_context/execution_context.cc

#ifndef MITTO_DOM_ERROR_REPORTING_H_
#define MITTO_DOM_ERROR_REPORTING_H_

#include "base/functional/function_ref.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-value.h"

namespace mitto {

// Tells the console of an error that no listener canceled.
using ReportToConsole = base::FunctionRef<void(v8::Local<v8::Value> exception)>;

// Reports `exception`, which nothing caught, and of which V8 made `message`,
// as Blink's MessageHandlerInMainThread() and DispatchErrorEvent() do: fires
// an ErrorEvent at the window of the isolate's current context, with the
// message, place and error that `message` gives, and then calls `to_console`
// unless a listener canceled it. An error thrown while the window's error
// listeners run goes to the console alone, after the one they run for. Without
// a window, as with no context, it goes to the console alone.
void ReportUncaughtError(v8::Isolate* isolate,
                         v8::Local<v8::Message> message,
                         v8::Local<v8::Value> exception,
                         ReportToConsole to_console);

// Whether the rejection of `reason`, of a promise rejected with no handler,
// fires events at the window, as Blink decides for each from V8's message of
// the reason: only for a script whose errors are not muted.
bool RejectionFiresEvents(v8::Isolate* isolate, v8::Local<v8::Value> reason);

// Fires unhandledrejection at the window of the current context for
// `promise`, rejected with `reason` and still with no handler, and returns
// whether no listener canceled it, which leaves it to the console.
bool DispatchUnhandledRejection(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise,
                                v8::Local<v8::Value> reason);

// The ScriptOrigin to compile a classic script from the URL `name` with, as
// Blink's ClassicScript::CreateScriptOrigin() makes it: shared cross-origin,
// so that the window's error listeners see its errors as they are, unless
// its errors are `muted`, as for a script of another origin whose response
// had opaque tainting. An inline script starts at `line_offset` and
// `column_offset` of its document, zero-based, so that its errors count
// lines and columns from the document's start.
v8::ScriptOrigin ClassicScriptOrigin(v8::Local<v8::Value> name,
                                     bool muted_errors,
                                     int line_offset = 0,
                                     int column_offset = 0);

// Reports what a script that the Runtime ran threw, which `try_catch`
// caught, as an error that nothing caught: the window's error event, then the
// console, with V8's message of it, which says where it was thrown. Nothing
// for one that ends the script's execution.
void ReportScriptException(v8::Isolate* isolate, const v8::TryCatch& try_catch);

// Fires rejectionhandled at the window of the current context, once a handler
// has come for `promise` after its unhandledrejection.
void DispatchRejectionHandled(v8::Isolate* isolate,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> reason);

}  // namespace mitto

#endif  // MITTO_DOM_ERROR_REPORTING_H_
