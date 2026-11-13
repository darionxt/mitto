// The HTML Standard's BroadcastChannel: what one channel posts reaches the
// others of its name and storage key, in this Page and in the Session's other
// Pages, through the Page's BroadcastChannelService. A channel stays alive
// while it is connected and a page listens for its messages, and disconnects
// when it closes or its window's context goes.
//
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.h
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.cc

#ifndef MITTO_DOM_BROADCAST_CHANNEL_H_
#define MITTO_DOM_BROADCAST_CHANNEL_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class SerializedScriptValue;
class Window;

class BroadcastChannel final : public EventTarget {
 public:
  // `new BroadcastChannel(name)`, in the current window.
  static BroadcastChannel* Create(const std::u16string& name);

  BroadcastChannel(Window* window, const std::u16string& name);
  ~BroadcastChannel() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return name_; }
  void PostMessage(v8::Local<v8::Value> message,
                   bindings::ExceptionState& exception_state);
  void Close();

  // A message of another channel, in a task of the window's context: a
  // message event, or a messageerror event if it is locked to another agent
  // cluster.
  void OnMessage(SerializedScriptValue message,
                 bool locked,
                 bool same_agent_cluster);

 protected:
  void ListenersChanged(std::u16string_view type) override;

 private:
  void CloseInternal();
  // HasPendingActivity(): connected, and a page listens for messages.
  void UpdateKeepAlive();

  const cppgc::WeakMember<Window> window_;
  const std::u16string name_;
  // The connection's ID, or 0 for none.
  uint64_t connection_ = 0;
  bool explicitly_closed_ = false;
  cppgc::Persistent<BroadcastChannel> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BROADCAST_CHANNEL_H_
