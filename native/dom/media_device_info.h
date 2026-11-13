// Media Capture and Streams' MediaDeviceInfo: an entry of enumerateDevices(),
// which a page with no permission to capture gets with an empty ID, group and
// label.
//
// Upstream: third_party/blink/renderer/modules/mediastream/media_device_info.cc

#ifndef MITTO_DOM_MEDIA_DEVICE_INFO_H_
#define MITTO_DOM_MEDIA_DEVICE_INFO_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace mitto {

class MediaDeviceInfo : public bindings::ScriptWrappable {
 public:
  // An entry of `kind`, "audioinput", "videoinput" or "audiooutput", with an
  // empty ID, group and label: an InputDeviceInfo for the two that record.
  static MediaDeviceInfo* Create(const std::u16string& kind);

  explicit MediaDeviceInfo(const std::u16string& kind);
  ~MediaDeviceInfo() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string DeviceId() const { return std::u16string(); }
  std::u16string Kind() const { return kind_; }
  std::u16string Label() const { return std::u16string(); }
  std::u16string GroupId() const { return std::u16string(); }
  v8::Local<v8::Object> ToJSONForBinding() const;

 private:
  const std::u16string kind_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MEDIA_DEVICE_INFO_H_
