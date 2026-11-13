#include "mitto/dom/permission_status.h"

#include "mitto/bindings/v8_permission_status.h"

namespace mitto {

// static
PermissionStatus* PermissionStatus::Create(const std::u16string& name,
                                           const std::u16string& state) {
  return bindings::MakeGarbageCollected<PermissionStatus>(name, state);
}

PermissionStatus::PermissionStatus(const std::u16string& name,
                                   const std::u16string& state)
    : name_(name), state_(state) {}

PermissionStatus::~PermissionStatus() = default;

const bindings::WrapperTypeInfo* PermissionStatus::GetWrapperTypeInfo() const {
  return &bindings::V8PermissionStatus::kWrapperTypeInfo;
}

}  // namespace mitto
