#include "mitto/dom/idb_version_change_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_idb_version_change_event.h"

namespace mitto {

// static
IDBVersionChangeEvent* IDBVersionChangeEvent::Create(
    const std::u16string& type,
    const bindings::IDBVersionChangeEventInit& init) {
  // The init dictionary's bubbles, cancelable and composed count for nothing.
  return bindings::MakeGarbageCollected<IDBVersionChangeEvent>(
      type, init.old_version, init.new_version, init.data_loss == u"total",
      std::u16string());
}

// static
IDBVersionChangeEvent* IDBVersionChangeEvent::CreateTrusted(
    const std::u16string& type,
    uint64_t old_version,
    std::optional<uint64_t> new_version,
    bool data_loss,
    const std::u16string& data_loss_message) {
  IDBVersionChangeEvent* const event =
      bindings::MakeGarbageCollected<IDBVersionChangeEvent>(
          type, old_version, new_version, data_loss, data_loss_message);
  event->SetTrusted(true);
  return event;
}

IDBVersionChangeEvent::IDBVersionChangeEvent(
    const std::u16string& type,
    uint64_t old_version,
    std::optional<uint64_t> new_version,
    bool data_loss,
    const std::u16string& data_loss_message)
    : Event(type, bindings::EventInit()),
      old_version_(old_version),
      new_version_(new_version),
      data_loss_(data_loss),
      data_loss_message_(data_loss_message) {}

IDBVersionChangeEvent::~IDBVersionChangeEvent() = default;

const bindings::WrapperTypeInfo* IDBVersionChangeEvent::GetWrapperTypeInfo()
    const {
  return &bindings::V8IDBVersionChangeEvent::kWrapperTypeInfo;
}

}  // namespace mitto
