#include "mitto/dom/media_devices.h"

#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "mitto/bindings/v8_media_devices.h"
#include "mitto/dom/media_device_info.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
MediaDevices* MediaDevices::Create(const PageMediaDevices& devices) {
  return bindings::MakeGarbageCollected<MediaDevices>(devices);
}

MediaDevices::MediaDevices(const PageMediaDevices& devices)
    : devices_(devices) {}

MediaDevices::~MediaDevices() = default;

const bindings::WrapperTypeInfo* MediaDevices::GetWrapperTypeInfo() const {
  return &bindings::V8MediaDevices::kWrapperTypeInfo;
}

v8::Local<v8::Promise> MediaDevices::EnumerateDevices(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // The browser's MediaDevicesManager gives a page that may not capture the
  // first device of each kind, in the order of MediaDeviceType, with no ID,
  // group or label, and none of a kind that the computer lacks.
  std::vector<std::u16string> kinds;
  if (devices_.audio_input) {
    kinds.push_back(u"audioinput");
  }
  if (devices_.video_input) {
    kinds.push_back(u"videoinput");
  }
  if (devices_.audio_output) {
    kinds.push_back(u"audiooutput");
  }
  ResolveFromTaskWith(
      isolate, resolver,
      base::BindOnce(
          [](const std::vector<std::u16string>& kinds,
             v8::Isolate* isolate) -> v8::Local<v8::Value> {
            std::vector<v8::Local<v8::Value>> entries;
            for (const std::u16string& kind : kinds) {
              entries.push_back(MediaDeviceInfo::Create(kind)->ToV8(isolate));
            }
            return v8::Array::New(isolate, entries.data(), entries.size());
          },
          kinds));
  return resolver->GetPromise();
}

}  // namespace mitto
