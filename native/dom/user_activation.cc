#include "mitto/dom/user_activation.h"

#include "mitto/bindings/v8_user_activation.h"

namespace mitto {

// static
UserActivation* UserActivation::CreateSnapshot() {
  return bindings::MakeGarbageCollected<UserActivation>(
      /*has_been_active=*/false, /*is_active=*/false);
}

// static
bindings::ScriptWrappable* UserActivation::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<UserActivation>(
      info, bindings::V8UserActivation::kWrapperTypeInfo,
      /*has_been_active=*/false, /*is_active=*/false);
}

UserActivation::UserActivation(bool has_been_active, bool is_active)
    : has_been_active_(has_been_active), is_active_(is_active) {}

UserActivation::~UserActivation() = default;

const bindings::WrapperTypeInfo* UserActivation::GetWrapperTypeInfo() const {
  return &bindings::V8UserActivation::kWrapperTypeInfo;
}

}  // namespace mitto
