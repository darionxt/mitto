#include "mitto/dom/message_event.h"

#include <utility>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_message_event.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/user_activation.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// IsValidSource(): a window or a port, as a service worker has no object in a
// Page.
bool IsValidSource(const EventTarget* source) {
  if (!source) {
    return true;
  }
  const bindings::WrapperTypeInfo* const info = source->GetWrapperTypeInfo();
  return info == &bindings::V8Window::kWrapperTypeInfo ||
         info == &bindings::V8MessagePort::kWrapperTypeInfo;
}

bindings::EventInit NotBubblingNotCancelable() {
  return bindings::EventInit();
}

}  // namespace

// static
MessageEvent* MessageEvent::Create(const std::u16string& type,
                                   const bindings::MessageEventInit& init,
                                   bindings::ExceptionState& exception_state) {
  if (!IsValidSource(init.source)) {
    exception_state.ThrowTypeError(
        "The optional 'source' property is neither a Window nor MessagePort.");
    return nullptr;
  }
  return bindings::MakeGarbageCollected<MessageEvent>(type, init);
}

// static
MessageEvent* MessageEvent::CreateForMessage(
    SerializedScriptValue data,
    std::optional<std::u16string> origin,
    EventTarget* source,
    std::vector<MessagePort*> ports,
    mitto::UserActivation* user_activation) {
  auto* const event = bindings::MakeGarbageCollected<MessageEvent>(
      u"message", std::move(data), std::move(origin), source, std::move(ports),
      user_activation);
  event->SetTrusted(true);
  return event;
}

// static
MessageEvent* MessageEvent::CreateError(std::optional<std::u16string> origin) {
  auto* const event = bindings::MakeGarbageCollected<MessageEvent>(
      u"messageerror", std::nullopt, std::move(origin), nullptr,
      std::vector<MessagePort*>(), nullptr);
  event->SetTrusted(true);
  return event;
}

MessageEvent::MessageEvent(const std::u16string& type,
                           const bindings::MessageEventInit& init)
    : Event(type, init),
      origin_(init.origin),
      last_event_id_(init.last_event_id),
      source_(init.source),
      user_activation_(init.user_activation) {
  // Blink keeps no data for null or undefined.
  if (!init.data.IsEmpty() && !init.data->IsNullOrUndefined()) {
    data_.Reset(v8::Isolate::GetCurrent(), init.data);
  }
  SetPorts(init.ports);
}

MessageEvent::MessageEvent(const std::u16string& type,
                           std::optional<SerializedScriptValue> data,
                           std::optional<std::u16string> origin,
                           EventTarget* source,
                           std::vector<MessagePort*> ports,
                           mitto::UserActivation* user_activation)
    : Event(type, NotBubblingNotCancelable()),
      serialized_data_(std::move(data)),
      origin_(origin.value_or(std::u16string())),
      source_(source),
      user_activation_(user_activation) {
  SetPorts(ports);
}

MessageEvent::~MessageEvent() = default;

const bindings::WrapperTypeInfo* MessageEvent::GetWrapperTypeInfo() const {
  return &bindings::V8MessageEvent::kWrapperTypeInfo;
}

void MessageEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(data_);
  visitor->Trace(source_);
  ports_.Trace(visitor);
  visitor->Trace(ports_array_);
  visitor->Trace(user_activation_);
}

v8::Local<v8::Value> MessageEvent::Data() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (serialized_data_ && !data_read_) {
    data_read_ = true;
    data_.Reset(isolate,
                serialized_data_->Deserialize(isolate, ports_.ToVector()));
  }
  if (data_.IsEmpty()) {
    return v8::Null(isolate);
  }
  return data_.Get(isolate);
}

v8::Local<v8::Value> MessageEvent::Ports() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (ports_array_.IsEmpty()) {
    std::vector<v8::Local<v8::Value>> wrappers;
    for (MessagePort* const port : ports_.ToVector()) {
      wrappers.push_back(port->ToV8(isolate));
    }
    ports_array_.Reset(isolate, FrozenArray(isolate, wrappers));
  }
  return ports_array_.Get(isolate);
}

void MessageEvent::InitMessageEvent(const std::u16string& type,
                                    bool bubbles,
                                    bool cancelable,
                                    v8::Local<v8::Value> data,
                                    const std::u16string& origin,
                                    const std::u16string& last_event_id,
                                    EventTarget* source,
                                    const std::vector<MessagePort*>& ports) {
  if (IsBeingDispatched()) {
    return;
  }
  InitEvent(type, bubbles, cancelable);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  serialized_data_.reset();
  data_read_ = false;
  data_.Reset(isolate, data);
  origin_ = origin;
  last_event_id_ = last_event_id;
  source_ = source;
  SetPorts(ports);
}

void MessageEvent::SetPorts(const std::vector<MessagePort*>& ports) {
  ports_.clear();
  for (MessagePort* const port : ports) {
    ports_.push_back(port);
  }
  ports_array_.Reset();
}

}  // namespace mitto
