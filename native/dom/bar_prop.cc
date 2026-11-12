#include "mitto/dom/bar_prop.h"

#include "mitto/bindings/v8_bar_prop.h"

namespace mitto {

// static
BarProp* BarProp::Create() {
  return bindings::MakeGarbageCollected<BarProp>();
}

// static
bindings::ScriptWrappable* BarProp::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<BarProp>(
      info, bindings::V8BarProp::kWrapperTypeInfo);
}

BarProp::BarProp() = default;
BarProp::~BarProp() = default;

const bindings::WrapperTypeInfo* BarProp::GetWrapperTypeInfo() const {
  return &bindings::V8BarProp::kWrapperTypeInfo;
}

}  // namespace mitto
