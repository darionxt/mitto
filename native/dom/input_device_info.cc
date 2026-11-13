#include "mitto/dom/input_device_info.h"

#include "mitto/bindings/v8_input_device_info.h"

namespace mitto {

InputDeviceInfo::InputDeviceInfo(const std::u16string& kind)
    : MediaDeviceInfo(kind) {}

InputDeviceInfo::~InputDeviceInfo() = default;

const bindings::WrapperTypeInfo* InputDeviceInfo::GetWrapperTypeInfo() const {
  return &bindings::V8InputDeviceInfo::kWrapperTypeInfo;
}

}  // namespace mitto
