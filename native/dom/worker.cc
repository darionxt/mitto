#include "mitto/dom/worker.h"

#include <optional>
#include <string>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_worker.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/serialized_script_value.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {

namespace {

// SecurityOrigin::CanReadContent() for a worker's script: one of the
// context's own origin, or a data: URL, whose worker gets an opaque origin.
bool CanReadContent(const ExecutionContext& context, const GURL& url) {
  if (url.SchemeIs(url::kDataScheme)) {
    return true;
  }
  return context.GetSecurityOrigin().IsSameOriginWith(url::Origin::Create(url));
}

}  // namespace

// static
Worker* Worker::Create(const std::u16string& script_url,
                       const bindings::WorkerOptions& options,
                       bindings::ExceptionState& exception_state) {
  ExecutionContext& context = ExecutionContext::Current();
  // DedicatedWorker::Create().
  if (!context.HasContext()) {
    exception_state.ThrowDOMException("InvalidAccessError",
                                      "The context provided is invalid.");
    return nullptr;
  }
  // AbstractWorker::ResolveURL(). A URL of another origin throws, as
  // NoSynchronousThrowForCrossOriginBlockedWorker is off for stable.
  const GURL url = context.CompleteUrl(script_url);
  if (!url.is_valid()) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(script_url),
                                     "' is not a valid URL."}));
    return nullptr;
  }
  if (!CanReadContent(context, url)) {
    exception_state.ThrowDOMException(
        "SecurityError",
        base::StrCat({"Script at '", ElidedUrl(url),
                      "' cannot be accessed from origin '",
                      context.GetSecurityOrigin().Serialize(), "'."}));
    return nullptr;
  }
  Worker* const worker = bindings::MakeGarbageCollected<Worker>();
  worker->Start(url, options);
  return worker;
}

Worker::Worker() = default;
Worker::~Worker() = default;

const bindings::WrapperTypeInfo* Worker::GetWrapperTypeInfo() const {
  return &bindings::V8Worker::kWrapperTypeInfo;
}

void Worker::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
}

void Worker::Start(const GURL& url, const bindings::WorkerOptions& options) {
  ExecutionContext& context = ExecutionContext::Current();
  WorkerStartOptions start;
  start.module = options.type == u"module";
  // A classic worker's script always goes with credentials same-origin.
  if (start.module) {
    if (options.credentials == u"omit") {
      start.credentials = FetchCredentialsMode::kOmit;
    } else if (options.credentials == u"include") {
      start.credentials = FetchCredentialsMode::kInclude;
    }
  }
  start.name = options.name;
  keep_alive_ = this;
  // DedicatedWorker::ContextDestroyed() terminates it.
  context.AddContextDestroyedCallback(base::BindOnce(
      [](cppgc::WeakPersistent<Worker> worker) {
        if (worker) {
          worker->Terminate();
        }
      },
      cppgc::WeakPersistent<Worker>(this)));
  if (WorkerStarter* const starter = context.GetWorkerStarter()) {
    proxy_ = starter->StartWorker(*this, url, start);
  }
}

void Worker::Terminate() {
  if (terminated_) {
    return;
  }
  terminated_ = true;
  if (proxy_) {
    proxy_->Terminate();
  }
  keep_alive_.Clear();
}

void Worker::PostMessage(v8::Local<v8::Value> message,
                         const std::vector<v8::Local<v8::Object>>& transfer,
                         bindings::ExceptionState& exception_state) {
  bindings::PostMessageOptions options;
  options.transfer = transfer;
  PostMessage(message, options, exception_state);
}

void Worker::PostMessage(v8::Local<v8::Value> message,
                         const bindings::PostMessageOptions& options,
                         bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!ExecutionContext::Current().HasContext()) {
    return;
  }
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(isolate, message, options.transfer,
                                          transferables);
  if (!serialized) {
    return;
  }
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, transferables.message_ports);
  if (!channels) {
    return;
  }
  // A message after terminate() goes nowhere, once serialized. A window's
  // carries a snapshot of its user activation if asked, and a worker's none,
  // as PostMessageHelper::CreateUserActivationSnapshot() takes one only of a
  // window.
  if (proxy_) {
    proxy_->PostMessageToWorkerGlobalScope(
        *std::move(serialized), *std::move(channels),
        options.include_user_activation &&
            ExecutionContext::Current().IsWindow());
  }
}

void Worker::DispatchMessage(SerializedScriptValue message,
                             std::vector<MessagePortChannel> channels) {
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(&ExecutionContext::Current(),
                                 std::move(channels));
  // From the worker, of the creator's origin: no origin, no source.
  DispatchEvent(MessageEvent::CreateForMessage(
      std::move(message), /*origin=*/std::nullopt, /*source=*/nullptr,
      MessagePort::PortsOf(ports), /*user_activation=*/nullptr));
}

bool Worker::DispatchErrorEvent(ErrorEvent* event) {
  return DispatchEvent(event);
}

void Worker::DidFailToFetchScript() {
  bindings::EventInit init;
  init.cancelable = true;
  Event* const event = Event::Create(u"error", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

void Worker::DidEnd() {
  terminated_ = true;
  keep_alive_.Clear();
}

}  // namespace mitto
