#include "mitto/dom/notification.h"

#include "mitto/bindings/v8_notification.h"

namespace mitto {

// static
Notification* Notification::Create() {
  return bindings::MakeGarbageCollected<Notification>();
}

// static
bindings::ScriptWrappable* Notification::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Notification>(
      info, bindings::V8Notification::kWrapperTypeInfo);
}

Notification::Notification() = default;
Notification::~Notification() = default;

const bindings::WrapperTypeInfo* Notification::GetWrapperTypeInfo() const {
  return &bindings::V8Notification::kWrapperTypeInfo;
}

}  // namespace mitto
