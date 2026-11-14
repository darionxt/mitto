// The HTML Standard's MessageEvent, which window.postMessage(), MessagePort and
// BroadcastChannel fire, and which a page may make of its own. A message's
// data is deserialized the first time a page reads it, and the same value
// comes back after, as Blink's [CachedAttribute] keeps it, and so is its
// frozen array of ports.
//
// Upstream: third_party/blink/renderer/core/events/message_event.h
// Upstream: third_party/blink/renderer/core/events/message_event.cc

#ifndef MITTO_DOM_MESSAGE_EVENT_H_
#define MITTO_DOM_MESSAGE_EVENT_H_

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class EventTarget;
class MessagePort;
class UserActivation;

class MessageEvent final : public Event {
 public:
  // `new MessageEvent(type, init)`, or null after throwing a TypeError for a
  // source that is neither a window nor a port.
  static MessageEvent* Create(const std::u16string& type,
                              const bindings::MessageEventInit& init,
                              bindings::ExceptionState& exception_state);
  // The message event of a message, which neither bubbles nor can be
  // canceled: `origin` is the sender's, serialized, none for a port's.
  static MessageEvent* CreateForMessage(SerializedScriptValue data,
                                        std::optional<std::u16string> origin,
                                        EventTarget* source,
                                        std::vector<MessagePort*> ports,
                                        mitto::UserActivation* user_activation);
  // The messageerror event of a message that cannot be read where it came.
  static MessageEvent* CreateError(std::optional<std::u16string> origin);

  MessageEvent(const std::u16string& type,
               const bindings::MessageEventInit& init);
  MessageEvent(const std::u16string& type,
               std::optional<SerializedScriptValue> data,
               std::optional<std::u16string> origin,
               EventTarget* source,
               std::vector<MessagePort*> ports,
               mitto::UserActivation* user_activation);
  ~MessageEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Data();
  std::u16string OriginForBindings() const { return origin_; }
  std::u16string LastEventId() const { return last_event_id_; }
  EventTarget* Source() const { return source_.Get(); }
  v8::Local<v8::Value> Ports();
  mitto::UserActivation* UserActivation() const {
    return user_activation_.Get();
  }
  void InitMessageEvent(const std::u16string& type,
                        bool bubbles,
                        bool cancelable,
                        v8::Local<v8::Value> data,
                        const std::u16string& origin,
                        const std::u16string& last_event_id,
                        EventTarget* source,
                        const std::vector<MessagePort*>& ports);

 private:
  void SetPorts(const std::vector<MessagePort*>& ports);

  // The data a page gave, empty for null, or a message's, which is read the
  // first time into `data_`.
  v8::TracedReference<v8::Value> data_;
  std::optional<SerializedScriptValue> serialized_data_;
  bool data_read_ = false;
  std::u16string origin_;
  std::u16string last_event_id_;
  cppgc::Member<EventTarget> source_;
  HeapVector<MessagePort> ports_;
  v8::TracedReference<v8::Array> ports_array_;
  cppgc::Member<mitto::UserActivation> user_activation_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MESSAGE_EVENT_H_
