// Media Capture and Streams' InputDeviceInfo: an entry of enumerateDevices() of
// a device that records. Its getCapabilities() is a Known Gap.
//
// Upstream: third_party/blink/renderer/modules/mediastream/input_device_info.cc

#ifndef MITTO_DOM_INPUT_DEVICE_INFO_H_
#define MITTO_DOM_INPUT_DEVICE_INFO_H_

#include <string>

#include "mitto/dom/media_device_info.h"

namespace mitto {

class InputDeviceInfo final : public MediaDeviceInfo {
 public:
  explicit InputDeviceInfo(const std::u16string& kind);
  ~InputDeviceInfo() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_INPUT_DEVICE_INFO_H_
