// The HTML Standard's MessageChannel: two ports of the current window,
// entangled with each other.
//
// Upstream: third_party/blink/renderer/core/messaging/message_channel.cc

#ifndef MITTO_DOM_MESSAGE_CHANNEL_H_
#define MITTO_DOM_MESSAGE_CHANNEL_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class MessagePort;

class MessageChannel final : public bindings::ScriptWrappable {
 public:
  // `new MessageChannel()`.
  static MessageChannel* Create();

  MessageChannel();
  ~MessageChannel() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  MessagePort* Port1() const { return port1_.Get(); }
  MessagePort* Port2() const { return port2_.Get(); }

 private:
  const cppgc::Member<MessagePort> port1_;
  const cppgc::Member<MessagePort> port2_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MESSAGE_CHANNEL_H_
