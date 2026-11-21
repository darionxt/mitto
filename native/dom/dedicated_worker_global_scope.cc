#include "mitto/dom/dedicated_worker_global_scope.h"

#include <optional>
#include <utility>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_dedicated_worker_global_scope.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/serialized_script_value.h"

namespace mitto {

// static
DedicatedWorkerGlobalScope* DedicatedWorkerGlobalScope::Create(
    std::u16string name) {
  return bindings::MakeGarbageCollected<DedicatedWorkerGlobalScope>(
      std::move(name));
}

DedicatedWorkerGlobalScope::DedicatedWorkerGlobalScope(std::u16string name)
    : name_(std::move(name)) {}

DedicatedWorkerGlobalScope::~DedicatedWorkerGlobalScope() = default;

const bindings::WrapperTypeInfo*
DedicatedWorkerGlobalScope::GetWrapperTypeInfo() const {
  return &bindings::V8DedicatedWorkerGlobalScope::kWrapperTypeInfo;
}

void DedicatedWorkerGlobalScope::PostMessage(
    v8::Local<v8::Value> message,
    const std::vector<v8::Local<v8::Object>>& transfer,
    bindings::ExceptionState& exception_state) {
  bindings::PostMessageOptions options;
  options.transfer = transfer;
  PostMessage(message, options, exception_state);
}

void DedicatedWorkerGlobalScope::PostMessage(
    v8::Local<v8::Value> message,
    const bindings::PostMessageOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!HasContext()) {
    return;
  }
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(isolate, message, options.transfer,
                                          transferables);
  if (!serialized) {
    return;
  }
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, transferables.message_ports);
  if (!channels) {
    return;
  }
  // Even once close() has been called, as Blink posts it.
  if (object_proxy_) {
    object_proxy_->PostMessageToWorkerObject(*std::move(serialized),
                                             *std::move(channels));
  }
}

void DedicatedWorkerGlobalScope::ReceiveMessage(
    SerializedScriptValue message,
    std::vector<MessagePortChannel> channels) {
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(this, std::move(channels));
  // From the Worker, whose origin is the worker's: no origin, no source.
  DispatchEvent(MessageEvent::CreateForMessage(
      std::move(message), /*origin=*/std::nullopt, /*source=*/nullptr,
      MessagePort::PortsOf(ports), /*user_activation=*/nullptr));
}

}  // namespace mitto
