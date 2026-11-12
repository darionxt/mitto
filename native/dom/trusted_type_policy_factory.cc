#include "mitto/dom/trusted_type_policy_factory.h"

#include "mitto/bindings/v8_trusted_type_policy_factory.h"

namespace mitto {

// static
TrustedTypePolicyFactory* TrustedTypePolicyFactory::Create() {
  return bindings::MakeGarbageCollected<TrustedTypePolicyFactory>();
}

// static
bindings::ScriptWrappable* TrustedTypePolicyFactory::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<TrustedTypePolicyFactory>(
      info, bindings::V8TrustedTypePolicyFactory::kWrapperTypeInfo);
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory() = default;
TrustedTypePolicyFactory::~TrustedTypePolicyFactory() = default;

const bindings::WrapperTypeInfo* TrustedTypePolicyFactory::GetWrapperTypeInfo()
    const {
  return &bindings::V8TrustedTypePolicyFactory::kWrapperTypeInfo;
}

}  // namespace mitto
