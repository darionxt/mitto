#include "mitto/dom/custom_element_registry.h"

#include "mitto/bindings/v8_custom_element_registry.h"

namespace mitto {

// static
CustomElementRegistry* CustomElementRegistry::Create() {
  return bindings::MakeGarbageCollected<CustomElementRegistry>();
}

// static
bindings::ScriptWrappable* CustomElementRegistry::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<CustomElementRegistry>(
      info, bindings::V8CustomElementRegistry::kWrapperTypeInfo);
}

CustomElementRegistry::CustomElementRegistry() = default;
CustomElementRegistry::~CustomElementRegistry() = default;

const bindings::WrapperTypeInfo* CustomElementRegistry::GetWrapperTypeInfo()
    const {
  return &bindings::V8CustomElementRegistry::kWrapperTypeInfo;
}

}  // namespace mitto
