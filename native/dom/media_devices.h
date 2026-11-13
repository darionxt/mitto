// Media Capture and Streams' MediaDevices, navigator.mediaDevices:
// enumerateDevices() as Chrome answers a page with no permission to capture,
// one entry of each kind of device that the Machine has. Capturing and its
// constraints are Known Gaps, and the device change event never fires.
//
// Upstream: third_party/blink/renderer/modules/mediastream/media_devices.cc
// Upstream: content/browser/renderer_host/media/media_devices_manager.cc
// Upstream: content/browser/media/media_devices_util.cc

#ifndef MITTO_DOM_MEDIA_DEVICES_H_
#define MITTO_DOM_MEDIA_DEVICES_H_

#include "mitto/bindings/exception_state.h"
#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class MediaDevices final : public EventTarget {
 public:
  static MediaDevices* Create(const PageMediaDevices& devices);

  explicit MediaDevices(const PageMediaDevices& devices);
  ~MediaDevices() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: new entries each call, from a task of its
  // own, as the browser answers.
  v8::Local<v8::Promise> EnumerateDevices(
      bindings::ExceptionState& exception_state);

 private:
  const PageMediaDevices devices_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MEDIA_DEVICES_H_
