#include "mitto/dom/worker_global_scope.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_worker_global_scope.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

WorkerGlobalScope::WorkerGlobalScope() = default;
WorkerGlobalScope::~WorkerGlobalScope() = default;

const bindings::WrapperTypeInfo* WorkerGlobalScope::GetWrapperTypeInfo() const {
  return &bindings::V8WorkerGlobalScope::kWrapperTypeInfo;
}

void WorkerGlobalScope::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  ExecutionContext::Trace(visitor);
  visitor->Trace(performance_);
}

mitto::Performance* WorkerGlobalScope::Performance() {
  if (!performance_) {
    performance_ = Performance::CreateForWorker(
        bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
            ->time_origin());
  }
  return performance_.Get();
}

std::u16string WorkerGlobalScope::Origin() const {
  return base::UTF8ToUTF16(origin_.Serialize());
}

bool WorkerGlobalScope::IsSecureContextForBindings() const {
  // A worker is a secure context if its creator is, which the Page decides
  // for its isolate when it starts the worker.
  return bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
      ->secure_context();
}

// As Window::StructuredClone(), in the worker's context.
v8::Local<v8::Value> WorkerGlobalScope::StructuredClone(
    v8::Local<v8::Value> value,
    const bindings::StructuredSerializeOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!HasContext()) {
    return v8::Undefined(isolate);
  }
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(isolate, value, options.transfer,
                                          transferables);
  if (!serialized) {
    return v8::Undefined(isolate);
  }
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, transferables.message_ports);
  if (!channels) {
    return v8::Undefined(isolate);
  }
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(this, *std::move(channels));
  return serialized->Deserialize(isolate, MessagePort::PortsOf(ports));
}

std::u16string WorkerGlobalScope::Url() const {
  return base::UTF8ToUTF16(url_.possibly_invalid_spec());
}

EventTarget* WorkerGlobalScope::ErrorEventTarget() {
  return this;
}

url::Origin WorkerGlobalScope::GetSecurityOrigin() const {
  return origin_;
}

GURL WorkerGlobalScope::BaseUrl() const {
  return url_;
}

GURL WorkerGlobalScope::CompleteUrl(std::u16string_view url) const {
  // A worker's URLs are UTF-8, whatever its creator's document has.
  return url_.Resolve(url);
}

void WorkerGlobalScope::AssociateWithContext(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
