#include "mitto/dom/timestamp_trigger.h"

#include "mitto/bindings/v8_timestamp_trigger.h"

namespace mitto {

// static
TimestampTrigger* TimestampTrigger::Create() {
  return bindings::MakeGarbageCollected<TimestampTrigger>();
}

// static
bindings::ScriptWrappable* TimestampTrigger::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<TimestampTrigger>(
      info, bindings::V8TimestampTrigger::kWrapperTypeInfo);
}

TimestampTrigger::TimestampTrigger() = default;
TimestampTrigger::~TimestampTrigger() = default;

const bindings::WrapperTypeInfo* TimestampTrigger::GetWrapperTypeInfo() const {
  return &bindings::V8TimestampTrigger::kWrapperTypeInfo;
}

}  // namespace mitto
