// The HTML Standard's MessagePort, as Blink's runs it over a mojo message pipe,
// and the pipe itself, within the Page's process.
//
// Each port holds one end of a pipe. What a port posts goes to the other end,
// which keeps it until its port reads it. A port reads as mojo::Connector
// does: once it has started, the first message that comes while it waits
// posts a task, which dispatches that message alone, and then one task more
// for each message that waits, at the back of the queue. A port that closes
// drops what it has not read, and its peer closes itself once it has read the
// rest. A transferred port takes its end, and the messages waiting there, to
// the new port. A port sent through its own pipe to its peer can never be read,
// and its peer never hears that it went. A port stays alive while it has
// started and is entangled, and closes when its window's context goes.
//
// Upstream: third_party/blink/renderer/core/messaging/message_port.h
// Upstream: third_party/blink/renderer/core/messaging/message_port.cc
// Upstream:
// third_party/blink/renderer/bindings/core/v8/serialization/post_message_helper.cc
// Upstream: mojo/public/cpp/bindings/lib/connector.cc

#ifndef MITTO_DOM_MESSAGE_PORT_H_
#define MITTO_DOM_MESSAGE_PORT_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "build/build_config.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/serialized_script_value.h"
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

class MessagePortChannel;
class SchedulerTaskContext;
class Window;

// A message as it goes from port to port: Blink's BlinkTransferableMessage.
struct TransferableMessage {
  explicit TransferableMessage(SerializedScriptValue message);
  TransferableMessage(TransferableMessage&&);
  TransferableMessage& operator=(TransferableMessage&&);
  ~TransferableMessage();

  SerializedScriptValue message;
  std::vector<MessagePortChannel> ports;
  // Whether the message carries a snapshot of the sender's user activation.
  bool user_activation = false;
  // The scheduler's state of the task that posted it, which a port that is
  // still entangled with its first peer carries to the task that dispatches
  // it, as Blink's task attribution does for a channel used to schedule.
  cppgc::Persistent<SchedulerTaskContext> task_state;
};

// One end of a pipe between two ports: Blink's MessagePortDescriptor with its
// end of a mojo message pipe. It closes its end when it goes.
class MessagePortChannel {
 public:
  static std::pair<MessagePortChannel, MessagePortChannel> CreatePair();

  MessagePortChannel();
  MessagePortChannel(const MessagePortChannel&) = delete;
  MessagePortChannel& operator=(const MessagePortChannel&) = delete;
  MessagePortChannel(MessagePortChannel&&);
  MessagePortChannel& operator=(MessagePortChannel&&);
  ~MessagePortChannel();

  bool is_valid() const { return !!pipe_; }

  // Sends `message` to the other end, which keeps it until its port reads it,
  // or drops it if that end has closed.
  void Send(TransferableMessage message);
  // The next message that came to this end, if one waits.
  std::optional<TransferableMessage> Take();
  size_t PendingCount() const;
  // Whether the other end has closed.
  bool PeerClosed() const;
  // Runs `notify` once, when a message comes or the other end closes, as a
  // mojo trap does, and returns true. Returns false, and keeps nothing, if a
  // message waits or the other end has closed already.
  bool Arm(base::OnceClosure notify);
  // Closes this end: what waits here goes, and the other end hears of it.
  void Close();

 private:
  class Pipe;

  MessagePortChannel(scoped_refptr<Pipe> pipe, int side);

  scoped_refptr<Pipe> pipe_;
  int side_ = 0;
};

class MessagePort final : public EventTarget {
 public:
  // A port of `window`'s context, entangled with nothing yet. One made where
  // the context has gone starts out closed.
  explicit MessagePort(Window* window);
  ~MessagePort() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void PostMessage(v8::Local<v8::Value> message,
                   const std::vector<v8::Local<v8::Object>>& transfer,
                   bindings::ExceptionState& exception_state);
  void PostMessage(v8::Local<v8::Value> message,
                   const bindings::PostMessageOptions& options,
                   bindings::ExceptionState& exception_state);
  void Start();
  void Close();

  // Holds `channel`. `initially_entangled_port` is the port at its other end
  // when a MessageChannel makes both.
  void Entangle(MessagePortChannel channel,
                MessagePort* initially_entangled_port);
  // Gives up its end, which leaves the port neutered.
  MessagePortChannel Disentangle();
  bool IsNeutered() const { return !channel_.is_valid(); }
  bool IsEntangled() const { return !closed_ && !IsNeutered(); }

  // DisentanglePorts(): the ends of `ports`, which a message transfers, or
  // nullopt after throwing Chrome's DataCloneError for a port that is null,
  // neutered or given twice.
  static std::optional<std::vector<MessagePortChannel>> DisentanglePorts(
      v8::Isolate* isolate,
      const std::vector<cppgc::Persistent<MessagePort>>& ports);
  // EntanglePorts(): a new port of `window`'s context for each end.
  static std::vector<MessagePort*> EntanglePorts(
      Window* window,
      std::vector<MessagePortChannel> channels);

  // PostMessageHelper::SerializeMessageByMove(): `message` serialized with
  // `transfer`, whose objects go into `transferables`, or nullopt after
  // throwing.
  static std::optional<SerializedScriptValue> SerializeMessageByMove(
      v8::Isolate* isolate,
      v8::Local<v8::Value> message,
      const std::vector<v8::Local<v8::Object>>& transfer,
      Transferables& transferables);

 private:
  // What mojo::Connector does for the port.
  void WaitToReadMore();
  static void PostReadTask(cppgc::WeakPersistent<MessagePort> port,
                           uint64_t generation,
                           bool counted);
  static void RunReadTask(cppgc::WeakPersistent<MessagePort> port,
                          uint64_t generation,
                          bool counted);
  void ReadAvailableMessage();
  void DispatchMessageEvent(TransferableMessage message);
  // OnConnectionError(): the other end has closed and nothing waits.
  void OnConnectionError();
  void OnEntangledPortDisconnected();
  // Keeps the port alive while it has started and is entangled, as Blink's
  // HasPendingActivity() does.
  void UpdateKeepAlive();

  const cppgc::WeakMember<Window> window_;
  MessagePortChannel channel_;
  cppgc::WeakMember<MessagePort> initially_entangled_port_;
  bool started_ = false;
  bool closed_ = false;
  // The tasks that will each read a message, beside the one that a trap
  // posts.
  size_t pending_dispatch_tasks_ = 0;
  // Each time the port gives up its end, the tasks it posted for the old one
  // do nothing.
  uint64_t generation_ = 0;
  cppgc::Persistent<MessagePort> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MESSAGE_PORT_H_
