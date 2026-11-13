#include "mitto/dom/message_port.h"

#include <array>
#include <deque>
#include <set>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/user_activation.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace mitto {

TransferableMessage::TransferableMessage(SerializedScriptValue message)
    : message(std::move(message)) {}
TransferableMessage::TransferableMessage(TransferableMessage&&) = default;
TransferableMessage& TransferableMessage::operator=(TransferableMessage&&) =
    default;
TransferableMessage::~TransferableMessage() = default;

// Both ends of a pipe: what came to each and was not read yet, whether each
// has closed, and the trap that each has armed. The Page's ports all live on
// its main thread.
class MessagePortChannel::Pipe : public base::RefCounted<Pipe> {
 public:
  Pipe() { All().insert(this); }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  // Every pipe of the Page.
  static std::set<Pipe*>& All() {
    static base::NoDestructor<std::set<Pipe*>> pipes;
    return *pipes;
  }

  // Runs the trap that `side` armed, if it has one.
  void Notify(int side) {
    if (traps_[side]) {
      std::move(traps_[side]).Run();
    }
  }

  // Marks where the ends that `message` carries wait: in the queue of `side`.
  void MarkQueued(const TransferableMessage& message, int side) {
    for (const MessagePortChannel& port : message.ports) {
      if (port.pipe_) {
        port.pipe_->queued_in_[port.side_] = {this, side};
      }
    }
  }

  static void MarkTaken(const TransferableMessage& message) {
    for (const MessagePortChannel& port : message.ports) {
      if (port.pipe_) {
        port.pipe_->queued_in_[port.side_] = {nullptr, 0};
      }
    }
  }

  // The ends that nothing can read again: those that wait in the queue of an
  // end that nothing can read, as one sent through its own pipe, or two sent
  // each through the other's pipe, do. An end that no queue holds, which a
  // port holds or a message carries on its way, can be read, and so can the
  // ends that wait in its queue. What comes to an end that nothing can read
  // goes, and its other end never hears that it went, as with mojo handles
  // that travel in a cycle of pipes.
  static void DropUnreachable() {
    std::set<std::pair<Pipe*, int>> reachable;
    std::vector<std::pair<Pipe*, int>> pending;
    for (Pipe* const pipe : All()) {
      for (int side = 0; side < 2; ++side) {
        if (pipe->IsOpen(side) && !pipe->queued_in_[side].first) {
          pending.emplace_back(pipe, side);
        }
      }
    }
    while (!pending.empty()) {
      const auto [pipe, side] = pending.back();
      pending.pop_back();
      if (!reachable.insert({pipe, side}).second) {
        continue;
      }
      for (const TransferableMessage& message : pipe->queues_[side]) {
        for (const MessagePortChannel& port : message.ports) {
          if (port.pipe_ && port.pipe_->IsOpen(port.side_)) {
            pending.emplace_back(port.pipe_.get(), port.side_);
          }
        }
      }
    }
    // Their queues go only once every pipe has been looked at, since letting
    // go of them may free pipes.
    std::vector<std::deque<TransferableMessage>> dropped;
    for (Pipe* const pipe : All()) {
      for (int side = 0; side < 2; ++side) {
        if (pipe->IsOpen(side) && !reachable.contains({pipe, side})) {
          pipe->unreachable_[side] = true;
          pipe->traps_[side].Reset();
          dropped.emplace_back().swap(pipe->queues_[side]);
        }
      }
    }
  }

  bool IsOpen(int side) const { return !closed_[side] && !unreachable_[side]; }

  std::array<std::deque<TransferableMessage>, 2> queues_;
  std::array<bool, 2> closed_ = {false, false};
  // Whether nothing can read the end of a side again.
  std::array<bool, 2> unreachable_ = {false, false};
  std::array<base::OnceClosure, 2> traps_;
  // Where the end of each side waits, if a message that is queued carries it:
  // the pipe and the side whose queue holds that message.
  std::array<std::pair<Pipe*, int>, 2> queued_in_ = {
      {{nullptr, 0}, {nullptr, 0}}};

 private:
  friend class base::RefCounted<Pipe>;
  ~Pipe() { All().erase(this); }
};

// static
std::pair<MessagePortChannel, MessagePortChannel>
MessagePortChannel::CreatePair() {
  auto pipe = base::MakeRefCounted<Pipe>();
  return {MessagePortChannel(pipe, 0), MessagePortChannel(pipe, 1)};
}

MessagePortChannel::MessagePortChannel() = default;

MessagePortChannel::MessagePortChannel(scoped_refptr<Pipe> pipe, int side)
    : pipe_(std::move(pipe)), side_(side) {}

MessagePortChannel::MessagePortChannel(MessagePortChannel&& other)
    : pipe_(std::move(other.pipe_)), side_(other.side_) {}

MessagePortChannel& MessagePortChannel::operator=(MessagePortChannel&& other) {
  if (this != &other) {
    Close();
    pipe_ = std::move(other.pipe_);
    side_ = other.side_;
  }
  return *this;
}

MessagePortChannel::~MessagePortChannel() {
  Close();
}

void MessagePortChannel::Send(TransferableMessage message) {
  if (!pipe_) {
    return;
  }
  const int peer = 1 - side_;
  if (!pipe_->IsOpen(peer)) {
    // Dropped, and with it the ends it carried.
    return;
  }
  const bool carries_ends = !message.ports.empty();
  pipe_->MarkQueued(message, peer);
  pipe_->queues_[peer].push_back(std::move(message));
  pipe_->Notify(peer);
  if (carries_ends) {
    Pipe::DropUnreachable();
  }
}

std::optional<TransferableMessage> MessagePortChannel::Take() {
  if (!pipe_ || pipe_->queues_[side_].empty()) {
    return std::nullopt;
  }
  TransferableMessage message = std::move(pipe_->queues_[side_].front());
  pipe_->queues_[side_].pop_front();
  Pipe::MarkTaken(message);
  return message;
}

size_t MessagePortChannel::PendingCount() const {
  return pipe_ ? pipe_->queues_[side_].size() : 0;
}

bool MessagePortChannel::PeerClosed() const {
  return !pipe_ || pipe_->closed_[1 - side_];
}

bool MessagePortChannel::Arm(base::OnceClosure notify) {
  if (!pipe_ || !pipe_->queues_[side_].empty() || PeerClosed()) {
    return false;
  }
  pipe_->traps_[side_] = std::move(notify);
  return true;
}

void MessagePortChannel::Close() {
  if (!pipe_) {
    return;
  }
  const scoped_refptr<Pipe> pipe = std::move(pipe_);
  // An end that nothing can read goes without its other end hearing of it.
  if (pipe->unreachable_[side_]) {
    return;
  }
  pipe->closed_[side_] = true;
  pipe->traps_[side_].Reset();
  // What waited here goes, closing the ends that it carried, which may reach
  // other pipes.
  std::deque<TransferableMessage> unread;
  unread.swap(pipe->queues_[side_]);
  unread.clear();
  pipe->Notify(1 - side_);
}

MessagePort::MessagePort(Window* window)
    : window_(window), closed_(!window || !window->HasContext()) {
  if (!closed_) {
    // ContextDestroyed() closes the port.
    window->AddContextDestroyedCallback(base::BindOnce(
        [](cppgc::WeakPersistent<MessagePort> port) {
          if (port) {
            port->Close();
          }
        },
        cppgc::WeakPersistent<MessagePort>(this)));
  }
}

MessagePort::~MessagePort() = default;

const bindings::WrapperTypeInfo* MessagePort::GetWrapperTypeInfo() const {
  return &bindings::V8MessagePort::kWrapperTypeInfo;
}

void MessagePort::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(initially_entangled_port_);
}

// static
std::optional<SerializedScriptValue> MessagePort::SerializeMessageByMove(
    v8::Isolate* isolate,
    v8::Local<v8::Value> message,
    const std::vector<v8::Local<v8::Object>>& transfer,
    Transferables& transferables) {
  if (!transfer.empty() && !SerializedScriptValue::ExtractTransferables(
                               isolate, transfer, transferables)) {
    return std::nullopt;
  }
  return SerializedScriptValue::Serialize(isolate, message, &transferables);
}

void MessagePort::PostMessage(
    v8::Local<v8::Value> message,
    const std::vector<v8::Local<v8::Object>>& transfer,
    bindings::ExceptionState& exception_state) {
  bindings::PostMessageOptions options;
  options.transfer = transfer;
  PostMessage(message, options, exception_state);
}

void MessagePort::PostMessage(v8::Local<v8::Value> message,
                              const bindings::PostMessageOptions& options,
                              bindings::ExceptionState& exception_state) {
  if (!IsEntangled()) {
    return;
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      SerializeMessageByMove(isolate, message, options.transfer, transferables);
  if (!serialized) {
    return;
  }
  // Not to the port it is sent from.
  for (size_t i = 0; i < transferables.message_ports.size(); ++i) {
    if (transferables.message_ports[i].Get() == this) {
      exception_state.ThrowDOMException(
          "DataCloneError",
          base::StrCat({"Port at index ", base::NumberToString(i),
                        " contains the source port."}));
      return;
    }
  }
  std::optional<std::vector<MessagePortChannel>> ports =
      DisentanglePorts(isolate, transferables.message_ports);
  if (!ports) {
    return;
  }
  TransferableMessage sent(*std::move(serialized));
  sent.ports = *std::move(ports);
  sent.user_activation = options.include_user_activation;
  if (initially_entangled_port_) {
    sent.task_state = SchedulerTaskContext::Current(isolate);
  }
  channel_.Send(std::move(sent));
}

void MessagePort::Start() {
  // Nothing if the port was transferred or closed.
  if (!IsEntangled() || started_) {
    return;
  }
  started_ = true;
  UpdateKeepAlive();
  WaitToReadMore();
}

void MessagePort::Close() {
  if (closed_) {
    return;
  }
  // A closed port is not neutered: it holds an end whose other end has gone.
  if (!IsNeutered()) {
    Disentangle();
    auto [ours, theirs] = MessagePortChannel::CreatePair();
    channel_ = std::move(ours);
  }
  closed_ = true;
  UpdateKeepAlive();
}

void MessagePort::Entangle(MessagePortChannel channel,
                           MessagePort* initially_entangled_port) {
  // In a context that has gone, a port never gets an end: nothing could reach
  // it.
  Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    return;
  }
  channel_ = std::move(channel);
  initially_entangled_port_ = initially_entangled_port;
}

MessagePortChannel MessagePort::Disentangle() {
  ++generation_;
  pending_dispatch_tasks_ = 0;
  if (MessagePort* const entangled = initially_entangled_port_.Get()) {
    entangled->OnEntangledPortDisconnected();
  }
  OnEntangledPortDisconnected();
  MessagePortChannel channel = std::move(channel_);
  UpdateKeepAlive();
  return channel;
}

void MessagePort::OnEntangledPortDisconnected() {
  initially_entangled_port_ = nullptr;
}

// static
std::optional<std::vector<MessagePortChannel>> MessagePort::DisentanglePorts(
    v8::Isolate* isolate,
    const std::vector<cppgc::Persistent<MessagePort>>& ports) {
  for (size_t i = 0; i < ports.size(); ++i) {
    MessagePort* const port = ports[i].Get();
    const char* type = nullptr;
    if (!port) {
      type = "null";
    } else if (port->IsNeutered()) {
      type = "already neutered";
    } else {
      for (size_t j = 0; j < i; ++j) {
        if (ports[j].Get() == port) {
          type = "a duplicate";
        }
      }
    }
    if (type) {
      bindings::ExceptionState exception_state(isolate);
      exception_state.ThrowDOMException(
          "DataCloneError",
          base::StrCat(
              {"Port at index ", base::NumberToString(i), " is ", type, "."}));
      return std::nullopt;
    }
  }
  std::vector<MessagePortChannel> channels;
  channels.reserve(ports.size());
  for (const cppgc::Persistent<MessagePort>& port : ports) {
    channels.push_back(port->Disentangle());
  }
  return channels;
}

// static
std::vector<cppgc::Persistent<MessagePort>> MessagePort::EntanglePorts(
    Window* window,
    std::vector<MessagePortChannel> channels) {
  std::vector<cppgc::Persistent<MessagePort>> ports;
  ports.reserve(channels.size());
  for (MessagePortChannel& channel : channels) {
    auto* const port = bindings::MakeGarbageCollected<MessagePort>(window);
    ports.emplace_back(port);
    port->Entangle(std::move(channel), nullptr);
  }
  return ports;
}

// static
std::vector<MessagePort*> MessagePort::PortsOf(
    const std::vector<cppgc::Persistent<MessagePort>>& ports) {
  std::vector<MessagePort*> raw;
  raw.reserve(ports.size());
  for (const cppgc::Persistent<MessagePort>& port : ports) {
    raw.push_back(port.Get());
  }
  return raw;
}

// Connector::WaitToReadMore() and ArmOrNotify(): a trap that posts a task when
// a message comes, or that task now if one waits. The trap may run while a
// port that the collector takes closes its end, so it only moves the handle it
// holds into a task.
void MessagePort::WaitToReadMore() {
  const bool armed = channel_.Arm(base::BindOnce(
      &MessagePort::PostReadTask, cppgc::WeakPersistent<MessagePort>(this),
      generation_, /*counted=*/false));
  if (!armed) {
    PostReadTask(cppgc::WeakPersistent<MessagePort>(this), generation_,
                 /*counted=*/false);
  }
}

// SimpleWatcher's task, which runs Connector::OnWatcherHandleReady(), or, when
// `counted`, Connector::PostDispatchNextMessageFromPipe()'s. Blink's port reads
// on TaskType::kPostedMessage, where this moves once the event loop has
// Blink's task types (#270).
// static
void MessagePort::PostReadTask(cppgc::WeakPersistent<MessagePort> port,
                               uint64_t generation,
                               bool counted) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(EventLoop::Queue::kDefault)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&MessagePort::RunReadTask, std::move(port),
                                generation, counted));
}

// static
void MessagePort::RunReadTask(cppgc::WeakPersistent<MessagePort> port,
                              uint64_t generation,
                              bool counted) {
  if (!port || port->generation_ != generation) {
    return;
  }
  if (counted) {
    --port->pending_dispatch_tasks_;
  }
  RunInWindowContext(port->window_.Get(),
                     [&] { port->ReadAvailableMessage(); });
}

// Connector::ReadAllAvailableMessages(), which dispatches one message for each
// task.
void MessagePort::ReadAvailableMessage() {
  if (!IsEntangled()) {
    return;
  }
  std::optional<TransferableMessage> message = channel_.Take();
  if (!message) {
    if (channel_.PeerClosed()) {
      OnConnectionError();
      return;
    }
    WaitToReadMore();
    return;
  }
  const uint64_t generation = generation_;
  DispatchMessageEvent(*std::move(message));
  // A listener may have closed or transferred the port.
  if (generation != generation_ || !IsEntangled()) {
    return;
  }
  const size_t pending = channel_.PendingCount();
  if (pending == 0) {
    WaitToReadMore();
    return;
  }
  while (pending > pending_dispatch_tasks_) {
    ++pending_dispatch_tasks_;
    PostReadTask(cppgc::WeakPersistent<MessagePort>(this), generation_,
                 /*counted=*/true);
  }
}

// DispatchMessageEvent() and CreateMessageEvent().
void MessagePort::DispatchMessageEvent(TransferableMessage message) {
  Window* const window = window_.Get();
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      EntanglePorts(window, std::move(message.ports));
  MessageEvent* const event = MessageEvent::CreateForMessage(
      std::move(message.message), /*origin=*/std::nullopt, /*source=*/nullptr,
      PortsOf(ports),
      message.user_activation ? UserActivation::CreateSnapshot() : nullptr);
  // The task state of a channel that schedules within the window.
  std::optional<SchedulerTaskScope> task_scope;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (initially_entangled_port_ && message.task_state &&
      !SchedulerTaskContext::Current(isolate)) {
    task_scope.emplace(isolate, message.task_state.Get());
  }
  DispatchEvent(event);
}

void MessagePort::OnConnectionError() {
  // The close event is behind MessagePortCloseEvent, which only tests turn on.
  Close();
}

void MessagePort::UpdateKeepAlive() {
  if (started_ && IsEntangled()) {
    keep_alive_ = this;
  } else {
    keep_alive_.Clear();
  }
}

}  // namespace mitto
