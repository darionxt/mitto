#include "mitto/dom/broadcast_channel.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_broadcast_channel.h"
#include "mitto/dom/broadcast_channel_service.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/window_task.h"
#include "mitto/dom/worker_global_scope.h"

namespace mitto {

// static
BroadcastChannel* BroadcastChannel::Create(const std::u16string& name) {
  return bindings::MakeGarbageCollected<BroadcastChannel>(
      &ExecutionContext::Current(), name);
}

BroadcastChannel::BroadcastChannel(ExecutionContext* context,
                                   const std::u16string& name)
    : context_(context), name_(name) {
  // A channel of a context that has gone stays unconnected, and its
  // postMessage() does nothing, as in a detached frame; so does one of a
  // worker that has called close(), as Blink connects none there.
  if (!context || !context->HasContext() || IsOfClosingWorker()) {
    return;
  }
  connection_ = BroadcastChannelService::Get().Connect(this, *context, name);
  context->AddContextDestroyedCallback(base::BindOnce(
      [](cppgc::WeakPersistent<BroadcastChannel> channel) {
        if (channel) {
          channel->CloseInternal();
        }
      },
      cppgc::WeakPersistent<BroadcastChannel>(this)));
}

// The service forgets the connection of a channel that nothing kept once it
// finds it gone.
BroadcastChannel::~BroadcastChannel() = default;

const bindings::WrapperTypeInfo* BroadcastChannel::GetWrapperTypeInfo() const {
  return &bindings::V8BroadcastChannel::kWrapperTypeInfo;
}

void BroadcastChannel::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(context_);
}

void BroadcastChannel::PostMessage(v8::Local<v8::Value> message,
                                   bindings::ExceptionState& exception_state) {
  if (!connection_) {
    if (explicitly_closed_) {
      exception_state.ThrowDOMException("InvalidStateError",
                                        "Channel is closed");
    }
    return;
  }
  // A worker that has called close() drops what its channels post, before
  // anything is serialized, as Blink's postMessage() does, since the
  // connection lasts until the worker goes.
  if (IsOfClosingWorker()) {
    return;
  }
  std::optional<SerializedScriptValue> serialized =
      SerializedScriptValue::Serialize(exception_state.GetIsolate(), message);
  if (!serialized) {
    return;
  }
  BroadcastChannelService::Get().Post(connection_, *serialized);
}

bool BroadcastChannel::IsOfClosingWorker() const {
  ExecutionContext* const context = context_.Get();
  return context && !context->IsWindow() &&
         static_cast<WorkerGlobalScope*>(context)->IsClosing();
}

void BroadcastChannel::Close() {
  explicitly_closed_ = true;
  CloseInternal();
}

void BroadcastChannel::CloseInternal() {
  if (connection_) {
    BroadcastChannelService::Get().Disconnect(connection_);
    connection_ = 0;
  }
  UpdateKeepAlive();
}

void BroadcastChannel::OnMessage(SerializedScriptValue message,
                                 bool locked,
                                 bool same_agent_cluster) {
  ExecutionContext* const context = context_.Get();
  if (!connection_ || !context) {
    return;
  }
  // The event's origin is the receiving context's, as the sender's is the
  // same.
  MessageEvent* const event =
      !locked || same_agent_cluster
          ? MessageEvent::CreateForMessage(
                std::move(message), context->Origin(),
                /*source=*/nullptr, std::vector<MessagePort*>(),
                /*user_activation=*/nullptr)
          : MessageEvent::CreateError(context->Origin());
  DispatchEvent(event);
}

void BroadcastChannel::DeliverInContext(SerializedScriptValue message,
                                        bool locked,
                                        bool same_agent_cluster) {
  RunInContext(context_.Get(), [&] {
    OnMessage(std::move(message), locked, same_agent_cluster);
  });
}

void BroadcastChannel::ListenersChanged(std::u16string_view type) {
  if (type == u"message") {
    UpdateKeepAlive();
  }
}

void BroadcastChannel::UpdateKeepAlive() {
  if (connection_ && HasEventListeners(u"message")) {
    keep_alive_ = this;
  } else {
    keep_alive_.Clear();
  }
}

}  // namespace mitto
