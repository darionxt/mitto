// The window's structuredClone() and postMessage(), as Blink's
// UniversalGlobalScope and DOMWindow run them. A message goes to the window
// that it is posted on from a task of that window's, as
// LocalDOMWindow::SchedulePostMessage() posts it, and names the window that
// posted it, the incumbent one, and that window's origin. One that a window
// posts to itself carries the task state of the task that posted it. One
// between windows that Chrome has in two processes goes as RemoteDOMWindow
// sends it, through the browser and LocalFrame::PostMessageEvent().
//
// Upstream: third_party/blink/renderer/core/frame/universal_global_scope.cc
// Upstream: third_party/blink/renderer/core/frame/dom_window.cc
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.cc
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/post_message_helper.cc

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/remote_window.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/user_activation.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Whom a message is for: any origin, or the one that the sender named, its own
// for "/".
struct TargetOrigin {
  bool any = false;
  url::Origin origin;
};

// PostMessageHelper::GetTargetOrigin(), or nullopt after throwing.
std::optional<TargetOrigin> GetTargetOrigin(
    const std::u16string& target_origin,
    const Window& source,
    bindings::ExceptionState& exception_state) {
  TargetOrigin target;
  if (target_origin == u"/") {
    target.origin = source.GetSecurityOrigin();
    return target;
  }
  if (target_origin == u"*") {
    target.any = true;
    return target;
  }
  target.origin = url::Origin::Create(GURL(target_origin));
  // No string can name an opaque origin.
  if (target.origin.opaque()) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"Invalid target origin '",
                                     base::UTF16ToUTF8(target_origin),
                                     "' in a call to 'postMessage'."}));
    return std::nullopt;
  }
  return target;
}

// PostMessageHelper::MapStringToDelegatedCapability(), for a window, which
// only checks the names: whether it threw.
bool CheckDelegatedCapability(const std::u16string& capability,
                              bindings::ExceptionState& exception_state) {
  const std::string text = base::UTF16ToUTF8(capability);
  for (std::string_view name : base::SplitStringPiece(
           text, " ", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    // CapabilityDelegationDigitalCredentials and CapabilityDelegationPopup
    // are off for stable.
    if (name == "payment" || name == "fullscreen" ||
        name == "display-capture") {
      return false;
    }
  }
  exception_state.ThrowDOMException(
      "NotSupportedError",
      base::StrCat({"Delegation of '", text, "' is not supported."}));
  return true;
}

// LocalDOMWindow::DispatchPostMessage() and
// DispatchMessageEventWithOriginCheck().
void DispatchPostMessage(Window& window,
                         SerializedScriptValue data,
                         std::vector<MessagePortChannel> channels,
                         Window* source,
                         RemoteWindow* remote_source,
                         const url::Origin& source_origin,
                         const TargetOrigin& target,
                         UserActivation* user_activation,
                         SchedulerTaskContext* task_state) {
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(&window, std::move(channels));
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::optional<SchedulerTaskScope> task_scope;
  if (task_state && !SchedulerTaskContext::Current(isolate)) {
    task_scope.emplace(isolate, task_state);
  }
  const url::Origin recipient = window.GetSecurityOrigin();
  const bool is_remote = !IsInSameProcess(source_origin, recipient);
  if (!target.any && !target.origin.IsSameOriginWith(recipient)) {
    // Between processes it is the browser that drops the message, without a
    // word, as RenderFrameProxyHost::RouteMessageEvent() does.
    if (!is_remote) {
      window.AddConsoleMessage(
          ConsoleLevel::kWarning,
          base::StrCat({"Failed to execute 'postMessage' on 'DOMWindow': The "
                        "target origin provided ('",
                        target.origin.Serialize(),
                        "') does not match the recipient window's origin ('",
                        recipient.Serialize(), "')."}));
    }
    return;
  }
  // A frame of another process that has gone, or that shows a document of this
  // one by now, is no source, as SourceFrameForOptionalToken() finds no remote
  // frame.
  if (is_remote && (!remote_source || remote_source->IsFrameDetached() ||
                    remote_source->IsReplaced())) {
    source = nullptr;
    remote_source = nullptr;
  }
  const std::u16string origin = base::UTF8ToUTF16(source_origin.Serialize());
  // A value locked to its agent cluster, a SharedArrayBuffer's or a
  // WebAssembly module's, is an error in another, and a window's cluster is
  // keyed by its origin, as Chrome keys it.
  if (data.IsLockedToAgentCluster() &&
      !source_origin.IsSameOriginWith(recipient)) {
    MessageEvent* const error = MessageEvent::CreateError(origin, source);
    error->SetRemoteSource(remote_source);
    window.DispatchEvent(error);
    return;
  }
  MessageEvent* const event = MessageEvent::CreateForMessage(
      std::move(data), origin, source, MessagePort::PortsOf(ports),
      user_activation);
  event->SetRemoteSource(remote_source);
  window.DispatchEvent(event);
}

// DOMWindow::DoPostMessage().
void DoPostMessage(Window& window,
                   SerializedScriptValue data,
                   const std::vector<cppgc::Persistent<MessagePort>>& ports,
                   const bindings::WindowPostMessageOptions& options,
                   Window& source,
                   bindings::ExceptionState& exception_state) {
  if (!window.HasContext()) {
    return;
  }
  // Worked out at once, for its SyntaxError.
  std::optional<TargetOrigin> target =
      GetTargetOrigin(options.target_origin, source, exception_state);
  if (!target) {
    return;
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, ports);
  if (!channels) {
    return;
  }
  UserActivation* const user_activation = options.include_user_activation
                                              ? UserActivation::CreateSnapshot()
                                              : nullptr;
  if (options.delegate) {
    if (CheckDelegatedCapability(options.delegate->value_or(std::u16string()),
                                 exception_state)) {
      return;
    }
    if (target->any) {
      exception_state.ThrowDOMException(
          "NotAllowedError", "Delegation to target origin '*' is not allowed.");
      return;
    }
    // A Page has no user activation.
    exception_state.ThrowDOMException(
        "NotAllowedError",
        "Delegation is not allowed without transient user activation.");
    return;
  }
  PostWindowTask(
      &window, TaskType::kPostedMessage,
      base::BindOnce(
          [](cppgc::WeakPersistent<Window> window, SerializedScriptValue data,
             std::vector<MessagePortChannel> channels,
             cppgc::Persistent<Window> source,
             cppgc::Persistent<RemoteWindow> remote_source,
             url::Origin source_origin, TargetOrigin target,
             cppgc::Persistent<UserActivation> user_activation,
             cppgc::Persistent<SchedulerTaskContext> task_state) {
            if (window) {
              DispatchPostMessage(*window, std::move(data), std::move(channels),
                                  source.Get(), remote_source.Get(),
                                  source_origin, target, user_activation.Get(),
                                  task_state.Get());
            }
          },
          cppgc::WeakPersistent<Window>(&window), std::move(data),
          *std::move(channels), cppgc::Persistent<Window>(&source),
          cppgc::Persistent<RemoteWindow>(
              RemoteWindow::From(source, window.GetSecurityOrigin())),
          source.GetSecurityOrigin(), *std::move(target),
          cppgc::Persistent<UserActivation>(user_activation),
          cppgc::Persistent<SchedulerTaskContext>(
              &source == &window ? SchedulerTaskContext::Current(isolate)
                                 : nullptr)));
}

}  // namespace

v8::Local<v8::Value> Window::StructuredClone(
    v8::Local<v8::Value> value,
    const bindings::StructuredSerializeOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  v8::Local<v8::Context> context;
  if (!HasContext() || !ContextOfGlobalProxy(isolate).ToLocal(&context)) {
    return v8::Undefined(isolate);
  }
  // UniversalGlobalScope::structuredClone() enters the window's own context,
  // whose objects the clone and what this throws are, whoever calls it.
  const v8::Context::Scope context_scope(context);
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(isolate, value, options.transfer,
                                          transferables);
  if (!serialized) {
    return v8::Undefined(isolate);
  }
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, transferables.message_ports);
  if (!channels) {
    return v8::Undefined(isolate);
  }
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(this, *std::move(channels));
  return serialized->Deserialize(isolate, MessagePort::PortsOf(ports));
}

void Window::PostMessage(v8::Local<v8::Value> message,
                         const std::u16string& target_origin,
                         const std::vector<v8::Local<v8::Object>>& transfer,
                         bindings::ExceptionState& exception_state) {
  bindings::WindowPostMessageOptions options;
  options.target_origin = target_origin;
  options.transfer = transfer;
  PostMessage(message, options, exception_state);
}

void Window::PostMessage(v8::Local<v8::Value> message,
                         const bindings::WindowPostMessageOptions& options,
                         bindings::ExceptionState& exception_state) {
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(exception_state.GetIsolate(), message,
                                          options.transfer, transferables);
  if (!serialized) {
    return;
  }
  // The incumbent window, whose script posts the message.
  Window* const incumbent = Incumbent();
  DoPostMessage(*this, *std::move(serialized), transferables.message_ports,
                options, incumbent ? *incumbent : *this, exception_state);
}

}  // namespace mitto
