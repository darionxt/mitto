#include "mitto/dom/permission_status.h"

#include <utility>

#include "mitto/bindings/v8_permission_status.h"

namespace mitto {

// static
PermissionStatus* PermissionStatus::Create(const std::u16string& name,
                                           scoped_refptr<SharedState> state) {
  return bindings::MakeGarbageCollected<PermissionStatus>(name,
                                                          std::move(state));
}

PermissionStatus::PermissionStatus(const std::u16string& name,
                                   scoped_refptr<SharedState> state)
    : name_(name), state_(std::move(state)) {}

PermissionStatus::~PermissionStatus() = default;

const bindings::WrapperTypeInfo* PermissionStatus::GetWrapperTypeInfo() const {
  return &bindings::V8PermissionStatus::kWrapperTypeInfo;
}

}  // namespace mitto
