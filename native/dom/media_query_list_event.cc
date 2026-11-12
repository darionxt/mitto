#include "mitto/dom/media_query_list_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_media_query_list_event.h"

namespace mitto {

// static
MediaQueryListEvent* MediaQueryListEvent::Create(
    const std::u16string& type,
    const bindings::MediaQueryListEventInit& init) {
  return bindings::MakeGarbageCollected<MediaQueryListEvent>(type, init);
}

MediaQueryListEvent::MediaQueryListEvent(
    const std::u16string& type,
    const bindings::MediaQueryListEventInit& init)
    : Event(type, init), media_(init.media), matches_(init.matches) {}

const bindings::WrapperTypeInfo* MediaQueryListEvent::GetWrapperTypeInfo()
    const {
  return &bindings::V8MediaQueryListEvent::kWrapperTypeInfo;
}

}  // namespace mitto
