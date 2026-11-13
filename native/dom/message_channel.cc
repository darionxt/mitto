#include "mitto/dom/message_channel.h"

#include <utility>

#include "mitto/bindings/v8_message_channel.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
MessageChannel* MessageChannel::Create() {
  return bindings::MakeGarbageCollected<MessageChannel>();
}

MessageChannel::MessageChannel()
    : port1_(bindings::MakeGarbageCollected<MessagePort>(&Window::Current())),
      port2_(bindings::MakeGarbageCollected<MessagePort>(&Window::Current())) {
  auto [first, second] = MessagePortChannel::CreatePair();
  port1_->Entangle(std::move(first), port2_.Get());
  port2_->Entangle(std::move(second), port1_.Get());
}

MessageChannel::~MessageChannel() = default;

const bindings::WrapperTypeInfo* MessageChannel::GetWrapperTypeInfo() const {
  return &bindings::V8MessageChannel::kWrapperTypeInfo;
}

void MessageChannel::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(port1_);
  visitor->Trace(port2_);
}

}  // namespace mitto
