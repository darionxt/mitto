#include "mitto/dom/storage_event.h"

#include <utility>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_storage_event.h"
#include "mitto/dom/storage.h"

namespace mitto {

// static
StorageEvent* StorageEvent::Create(const std::u16string& type,
                                   const bindings::StorageEventInit& init) {
  return bindings::MakeGarbageCollected<StorageEvent>(type, init);
}

// static
StorageEvent* StorageEvent::CreateForChange(
    const std::optional<std::u16string>& key,
    const std::optional<std::u16string>& old_value,
    const std::optional<std::u16string>& new_value,
    const std::u16string& url,
    Storage* storage_area) {
  auto* const event = bindings::MakeGarbageCollected<StorageEvent>(
      u"storage", bindings::EventInit(), key, old_value, new_value, url,
      storage_area);
  event->SetTrusted(true);
  return event;
}

StorageEvent::StorageEvent(const std::u16string& type,
                           const bindings::StorageEventInit& init)
    : Event(type, init),
      key_(init.key),
      old_value_(init.old_value),
      new_value_(init.new_value),
      url_(init.url),
      storage_area_(init.storage_area) {}

StorageEvent::StorageEvent(const std::u16string& type,
                           const bindings::EventInit& init,
                           std::optional<std::u16string> key,
                           std::optional<std::u16string> old_value,
                           std::optional<std::u16string> new_value,
                           std::u16string url,
                           Storage* storage_area)
    : Event(type, init),
      key_(std::move(key)),
      old_value_(std::move(old_value)),
      new_value_(std::move(new_value)),
      url_(std::move(url)),
      storage_area_(storage_area) {}

StorageEvent::~StorageEvent() = default;

const bindings::WrapperTypeInfo* StorageEvent::GetWrapperTypeInfo() const {
  return &bindings::V8StorageEvent::kWrapperTypeInfo;
}

void StorageEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(storage_area_);
}

void StorageEvent::InitStorageEvent(
    const std::u16string& type,
    bool bubbles,
    bool cancelable,
    const std::optional<std::u16string>& key,
    const std::optional<std::u16string>& old_value,
    const std::optional<std::u16string>& new_value,
    const std::u16string& url,
    mitto::Storage* storage_area) {
  if (IsBeingDispatched()) {
    return;
  }
  InitEvent(type, bubbles, cancelable);
  key_ = key;
  old_value_ = old_value;
  new_value_ = new_value;
  url_ = url;
  storage_area_ = storage_area;
}

}  // namespace mitto
