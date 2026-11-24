#include "mitto/dom/message_port.h"

#include <array>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/user_activation.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/cross-thread-persistent.h"
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

namespace {

// What every pipe's state is held under: a port's end and its peer's may be on
// two threads, the main thread's and a worker's, and a message may carry ends
// of other pipes.
base::Lock& PipeLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

}  // namespace

// Both ends of a pipe: what came to each and was not read yet, whether each
// has closed, and the trap that each has armed. An end belongs to the thread
// of its port, the main thread's or a worker's, which its trap posts to.
class MessagePortChannel::Pipe
    : public base::RefCountedThreadSafe<MessagePortChannel::Pipe> {
 public:
  Pipe() {
    const base::AutoLock lock(PipeLock());
    All().insert(this);
  }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  // Every pipe of the Page.
  static std::set<Pipe*>& All() EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
    static base::NoDestructor<std::set<Pipe*>> pipes;
    return *pipes;
  }

  // Posts the trap that `side` armed, if it has one, to its thread.
  void Notify(int side) EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
    if (traps_[side].task) {
      Trap trap = std::move(traps_[side]);
      trap.runner->PostTask(FROM_HERE, std::move(trap.task));
    }
  }

  // Marks where the ends that `message` carries wait: in the queue of `side`.
  void MarkQueued(const TransferableMessage& message, int side)
      EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
    for (const MessagePortChannel& port : message.ports) {
      if (port.pipe_) {
        port.pipe_->queued_in_[port.side_] = {this, side};
      }
    }
  }

  static void MarkTaken(const TransferableMessage& message)
      EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
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
  // that travel in a cycle of pipes. What goes is moved into `dropped`, for
  // the caller to let go of once it no longer holds the lock, since letting
  // go of a message closes the ends it carries.
  static void DropUnreachable(
      std::vector<std::deque<TransferableMessage>>& dropped)
      EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
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
    std::vector<Trap> traps;
    for (Pipe* const pipe : All()) {
      for (int side = 0; side < 2; ++side) {
        if (pipe->IsOpen(side) && !reachable.contains({pipe, side})) {
          pipe->unreachable_[side] = true;
          traps.push_back(std::move(pipe->traps_[side]));
          dropped.emplace_back().swap(pipe->queues_[side]);
        }
      }
    }
  }

  bool IsOpen(int side) const EXCLUSIVE_LOCKS_REQUIRED(PipeLock()) {
    return !closed_[side] && !unreachable_[side];
  }

  // A trap that an end armed: the task to post to its port's thread.
  struct Trap {
    scoped_refptr<base::SingleThreadTaskRunner> runner;
    base::OnceClosure task;
  };

  std::array<std::deque<TransferableMessage>, 2> queues_;
  std::array<bool, 2> closed_ = {false, false};
  // Whether nothing can read the end of a side again.
  std::array<bool, 2> unreachable_ = {false, false};
  std::array<Trap, 2> traps_;
  // Where the end of each side waits, if a message that is queued carries it:
  // the pipe and the side whose queue holds that message.
  std::array<std::pair<Pipe*, int>, 2> queued_in_ = {
      {{nullptr, 0}, {nullptr, 0}}};

 private:
  friend class base::RefCountedThreadSafe<Pipe>;
  ~Pipe() {
    const base::AutoLock lock(PipeLock());
    All().erase(this);
  }
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
  // Let go of only once the lock is not held.
  std::optional<TransferableMessage> not_sent;
  std::vector<std::deque<TransferableMessage>> dropped;
  {
    const base::AutoLock lock(PipeLock());
    const int peer = 1 - side_;
    if (!pipe_->IsOpen(peer)) {
      // Dropped, and with it the ends it carried.
      not_sent.emplace(std::move(message));
    } else {
      const bool carries_ends = !message.ports.empty();
      pipe_->MarkQueued(message, peer);
      pipe_->queues_[peer].push_back(std::move(message));
      pipe_->Notify(peer);
      if (carries_ends) {
        Pipe::DropUnreachable(dropped);
      }
    }
  }
}

std::optional<TransferableMessage> MessagePortChannel::Take() {
  if (!pipe_) {
    return std::nullopt;
  }
  const base::AutoLock lock(PipeLock());
  if (pipe_->queues_[side_].empty()) {
    return std::nullopt;
  }
  TransferableMessage message = std::move(pipe_->queues_[side_].front());
  pipe_->queues_[side_].pop_front();
  Pipe::MarkTaken(message);
  return message;
}

size_t MessagePortChannel::PendingCount() const {
  if (!pipe_) {
    return 0;
  }
  const base::AutoLock lock(PipeLock());
  return pipe_->queues_[side_].size();
}

bool MessagePortChannel::PeerClosed() const {
  if (!pipe_) {
    return true;
  }
  const base::AutoLock lock(PipeLock());
  return pipe_->closed_[1 - side_];
}

bool MessagePortChannel::Arm(scoped_refptr<base::SingleThreadTaskRunner> runner,
                             base::OnceClosure notify) {
  if (!pipe_) {
    return false;
  }
  const base::AutoLock lock(PipeLock());
  if (!pipe_->queues_[side_].empty() || pipe_->closed_[1 - side_]) {
    return false;
  }
  pipe_->traps_[side_] = {std::move(runner), std::move(notify)};
  return true;
}

void MessagePortChannel::Close() {
  if (!pipe_) {
    return;
  }
  // Declared before the lock, so that the pipe, and what waited here, go
  // once the lock is not held: they may close the ends that they carry.
  const scoped_refptr<Pipe> pipe = std::move(pipe_);
  std::deque<TransferableMessage> unread;
  Pipe::Trap trap;
  const base::AutoLock lock(PipeLock());
  // An end that nothing can read goes without its other end hearing of it.
  if (pipe->unreachable_[side_]) {
    return;
  }
  pipe->closed_[side_] = true;
  trap = std::move(pipe->traps_[side_]);
  unread.swap(pipe->queues_[side_]);
  pipe->Notify(1 - side_);
}

MessagePort::MessagePort(ExecutionContext* context)
    : context_(context), closed_(!context || !context->HasContext()) {
  if (!closed_) {
    // ContextDestroyed() closes the port.
    context->AddContextDestroyedCallback(base::BindOnce(
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
  visitor->Trace(context_);
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
  // Only a value that some contexts may not read is checked where it comes. A
  // port may outlive its context, whose value then no other may read.
  if (const ExecutionContext* const context = context_.Get();
      context && sent.message.IsLockedToAgentCluster()) {
    sent.sender_origin = context->GetSecurityOrigin();
    sent.sender_agent_cluster = context->AgentClusterOrigin();
  }
  // PostMessageHelper::CreateUserActivationSnapshot(): only a window's.
  sent.user_activation =
      options.include_user_activation && ExecutionContext::Current().IsWindow();
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
  ExecutionContext* const context = context_.Get();
  if (!context || !context->HasContext()) {
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
    ExecutionContext* context,
    std::vector<MessagePortChannel> channels) {
  std::vector<cppgc::Persistent<MessagePort>> ports;
  ports.reserve(channels.size());
  for (MessagePortChannel& channel : channels) {
    auto* const port = bindings::MakeGarbageCollected<MessagePort>(context);
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
  // The trap posts to this thread from the thread that sends, so it holds
  // the port by a handle that any thread may let go of.
  const bool armed = channel_.Arm(
      ReadTaskRunner(),
      base::BindOnce(
          &MessagePort::RunReadTask,
          cppgc::subtle::WeakCrossThreadPersistent<MessagePort>(this),
          generation_, /*counted=*/false));
  if (!armed) {
    PostReadTask(cppgc::subtle::WeakCrossThreadPersistent<MessagePort>(this),
                 generation_, /*counted=*/false);
  }
}

// static
scoped_refptr<base::SingleThreadTaskRunner> MessagePort::ReadTaskRunner() {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  return event_loop->TaskRunner(TaskType::kPostedMessage);
}

// SimpleWatcher's task, which runs Connector::OnWatcherHandleReady(), or, when
// `counted`, Connector::PostDispatchNextMessageFromPipe()'s, on
// TaskType::kPostedMessage, where Blink's port reads.
// static
void MessagePort::PostReadTask(
    cppgc::subtle::WeakCrossThreadPersistent<MessagePort> port,
    uint64_t generation,
    bool counted) {
  ReadTaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(&MessagePort::RunReadTask, std::move(port),
                                generation, counted));
}

// static
void MessagePort::RunReadTask(
    cppgc::subtle::WeakCrossThreadPersistent<MessagePort> port_handle,
    uint64_t generation,
    bool counted) {
  MessagePort* const port = port_handle.Get();
  if (!port || port->generation_ != generation) {
    return;
  }
  if (counted) {
    --port->pending_dispatch_tasks_;
  }
  RunInContext(port->context_.Get(), [&] { port->ReadAvailableMessage(); });
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
    PostReadTask(cppgc::subtle::WeakCrossThreadPersistent<MessagePort>(this),
                 generation_, /*counted=*/true);
  }
}

// CreateMessageEvent(): a messageerror event for a value that this context may
// not read, a WebAssembly module of another origin or a SharedArrayBuffer of
// another agent cluster, wherever the port was when it was sent.
MessageEvent* MessagePort::CreateMessageEvent(TransferableMessage& message) {
  ExecutionContext* const context = context_.Get();
  if ((message.message.IsOriginCheckRequired() &&
       !message.sender_origin.IsSameOriginWith(context->GetSecurityOrigin())) ||
      (message.message.IsLockedToAgentCluster() &&
       !message.sender_agent_cluster.IsSameOriginWith(
           context->AgentClusterOrigin()))) {
    return MessageEvent::CreateError(/*origin=*/std::nullopt);
  }
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      EntanglePorts(context, std::move(message.ports));
  return MessageEvent::CreateForMessage(
      std::move(message.message), /*origin=*/std::nullopt, /*source=*/nullptr,
      PortsOf(ports),
      message.user_activation ? UserActivation::CreateSnapshot() : nullptr);
}

void MessagePort::DispatchMessageEvent(TransferableMessage message) {
  MessageEvent* const event = CreateMessageEvent(message);
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
