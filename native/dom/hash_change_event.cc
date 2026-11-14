#include "mitto/dom/hash_change_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_hash_change_event.h"

namespace mitto {

// static
HashChangeEvent* HashChangeEvent::Create(
    const std::u16string& type,
    const bindings::HashChangeEventInit& init) {
  return bindings::MakeGarbageCollected<HashChangeEvent>(type, init);
}

// static
HashChangeEvent* HashChangeEvent::CreateTrusted(const std::u16string& old_url,
                                                const std::u16string& new_url) {
  bindings::HashChangeEventInit init;
  init.old_url = old_url;
  init.new_url = new_url;
  HashChangeEvent* const event = Create(u"hashchange", init);
  event->SetTrusted(true);
  return event;
}

HashChangeEvent::HashChangeEvent(const std::u16string& type,
                                 const bindings::HashChangeEventInit& init)
    : Event(type, init),
      old_url_(init.old_url.value_or(std::u16string())),
      new_url_(init.new_url.value_or(std::u16string())) {}

HashChangeEvent::~HashChangeEvent() = default;

const bindings::WrapperTypeInfo* HashChangeEvent::GetWrapperTypeInfo() const {
  return &bindings::V8HashChangeEvent::kWrapperTypeInfo;
}

}  // namespace mitto
