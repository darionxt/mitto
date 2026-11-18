// The HTML Standard's Worker, as Blink's DedicatedWorker: `new Worker()`
// checks the script's URL and starts a dedicated worker through its context's
// WorkerStarter, postMessage() and terminate() go to the worker, and the
// worker's messages and errors come back as events, each from a task of the
// creator's context. A Worker stays alive until it is terminated or its
// context goes, as Blink's HasPendingActivity() keeps it.
//
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker.h
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker.cc
// Upstream: third_party/blink/renderer/core/workers/abstract_worker.cc

#ifndef MITTO_DOM_WORKER_H_
#define MITTO_DOM_WORKER_H_

#include <memory>
#include <string>
#include <vector>

#include "build/build_config.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

#if BUILDFLAG(IS_WIN)
// base/win/windows_types.h defines PostMessage as PostMessageW, as <windows.h>
// does, which would rename the member wherever it came first. Once its guard
// has seen it, the name stays the member's.
#include "base/win/windows_types.h"
#undef PostMessage
#endif  // BUILDFLAG(IS_WIN)

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ErrorEvent;
class MessagePortChannel;
class SerializedScriptValue;
class WorkerMessagingProxy;

class Worker final : public EventTarget {
 public:
  // `new Worker(scriptURL, options)`, in the current context.
  static Worker* Create(const std::u16string& script_url,
                        const bindings::WorkerOptions& options,
                        bindings::ExceptionState& exception_state);

  Worker();
  ~Worker() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void Terminate();
  void PostMessage(v8::Local<v8::Value> message,
                   const std::vector<v8::Local<v8::Object>>& transfer,
                   bindings::ExceptionState& exception_state);
  void PostMessage(v8::Local<v8::Value> message,
                   const bindings::PostMessageOptions& options,
                   bindings::ExceptionState& exception_state);

  // What the worker's proxy hands it, each from a task of the creator's
  // context.
  // A message of the worker's, with the ports it transfers: a message event.
  void DispatchMessage(SerializedScriptValue message,
                       std::vector<MessagePortChannel> ports);
  // An error that the worker left uncaught, at this Worker, which returns
  // whether no listener canceled it.
  bool DispatchErrorEvent(ErrorEvent* event);
  // The worker's script did not come, or was not one to run: a plain error
  // event, as Blink's DidFailToFetchScript() fires.
  void DidFailToFetchScript();

 private:
  // Starts the worker, as DedicatedWorker::Start() does once the checks of
  // its URL have passed.
  void Start(const GURL& url, const bindings::WorkerOptions& options);

  std::unique_ptr<WorkerMessagingProxy> proxy_;
  bool terminated_ = false;
  // HasPendingActivity(): until it is terminated.
  cppgc::Persistent<Worker> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WORKER_H_
