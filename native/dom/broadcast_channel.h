// The HTML Standard's BroadcastChannel: what one channel posts reaches the
// others of its name and storage key, in this Page, its documents and its
// dedicated workers, and in the Session's other Pages, through the Page's
// BroadcastChannelService. A channel stays alive while it is connected and a
// page listens for its messages, and disconnects when it closes or its
// context goes.
//
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.h
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.cc

#ifndef MITTO_DOM_BROADCAST_CHANNEL_H_
#define MITTO_DOM_BROADCAST_CHANNEL_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "build/build_config.h"
#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/member.h"
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

class ExecutionContext;
class SerializedScriptValue;

class BroadcastChannel final : public EventTarget {
 public:
  // `new BroadcastChannel(name)`, in the current context.
  static BroadcastChannel* Create(const std::u16string& name);

  BroadcastChannel(ExecutionContext* context, const std::u16string& name);
  ~BroadcastChannel() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return name_; }
  void PostMessage(v8::Local<v8::Value> message,
                   bindings::ExceptionState& exception_state);
  void Close();

  // A message of another channel, in a task of the channel's context: a
  // message event, or a messageerror event if it is locked to another agent
  // cluster.
  void OnMessage(SerializedScriptValue message,
                 bool locked,
                 bool same_agent_cluster);
  // The same, run in the channel's context, if it still has one, from a task
  // of the channel's thread.
  void DeliverInContext(SerializedScriptValue message,
                        bool locked,
                        bool same_agent_cluster);

 protected:
  void ListenersChanged(std::u16string_view type) override;

 private:
  // Whether its context is a worker that has called close(), as Blink's
  // WorkerGlobalScope::IsClosing() says.
  bool IsOfClosingWorker() const;
  void CloseInternal();
  // HasPendingActivity(): connected, and a page listens for messages.
  void UpdateKeepAlive();

  const cppgc::WeakMember<ExecutionContext> context_;
  const std::u16string name_;
  // The connection's ID, or 0 for none.
  uint64_t connection_ = 0;
  bool explicitly_closed_ = false;
  cppgc::Persistent<BroadcastChannel> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BROADCAST_CHANNEL_H_
