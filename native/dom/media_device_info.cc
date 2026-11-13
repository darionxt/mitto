#include "mitto/dom/media_device_info.h"

#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_media_device_info.h"
#include "mitto/dom/input_device_info.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
MediaDeviceInfo* MediaDeviceInfo::Create(const std::u16string& kind) {
  if (kind == u"audiooutput") {
    return bindings::MakeGarbageCollected<MediaDeviceInfo>(kind);
  }
  return bindings::MakeGarbageCollected<InputDeviceInfo>(kind);
}

MediaDeviceInfo::MediaDeviceInfo(const std::u16string& kind) : kind_(kind) {}

MediaDeviceInfo::~MediaDeviceInfo() = default;

const bindings::WrapperTypeInfo* MediaDeviceInfo::GetWrapperTypeInfo() const {
  return &bindings::V8MediaDeviceInfo::kWrapperTypeInfo;
}

// MediaDeviceInfo::toJSONForBinding(): its four attributes, in the IDL's
// order.
v8::Local<v8::Object> MediaDeviceInfo::ToJSONForBinding() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return PlainObject(isolate,
                     {{"deviceId", bindings::ToV8String(isolate, DeviceId())},
                      {"kind", bindings::ToV8String(isolate, Kind())},
                      {"label", bindings::ToV8String(isolate, Label())},
                      {"groupId", bindings::ToV8String(isolate, GroupId())}});
}

}  // namespace mitto
