#include "mitto/dom/broadcast_channel.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_broadcast_channel.h"
#include "mitto/dom/broadcast_channel_service.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
BroadcastChannel* BroadcastChannel::Create(const std::u16string& name) {
  return bindings::MakeGarbageCollected<BroadcastChannel>(&Window::Current(),
                                                          name);
}

BroadcastChannel::BroadcastChannel(Window* window, const std::u16string& name)
    : window_(window), name_(name) {
  // A channel of a window that has lost its context stays unconnected, and
  // its postMessage() does nothing, as in a detached frame.
  if (!window || !window->HasContext()) {
    return;
  }
  connection_ = BroadcastChannelService::Get().Connect(this, *window, name);
  window->AddContextDestroyedCallback(base::BindOnce(
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
  visitor->Trace(window_);
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
  std::optional<SerializedScriptValue> serialized =
      SerializedScriptValue::Serialize(exception_state.GetIsolate(), message);
  if (!serialized) {
    return;
  }
  BroadcastChannelService::Get().Post(connection_, *serialized);
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
  Window* const window = window_.Get();
  if (!connection_ || !window) {
    return;
  }
  // The event's origin is the receiving context's, as the sender's is the
  // same.
  MessageEvent* const event =
      !locked || same_agent_cluster
          ? MessageEvent::CreateForMessage(std::move(message), window->Origin(),
                                           /*source=*/nullptr,
                                           std::vector<MessagePort*>(),
                                           /*user_activation=*/nullptr)
          : MessageEvent::CreateError(window->Origin());
  DispatchEvent(event);
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
