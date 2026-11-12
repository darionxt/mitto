// The Screen Orientation API's ScreenOrientation, `screen.orientation`: the
// Machine's display in its natural orientation, which Chrome on Windows cannot
// lock. Its event handler attribute is a Known Gap.
//
// Upstream: third_party/blink/renderer/modules/screen_orientation/screen_orientation.cc
// Upstream: third_party/blink/renderer/modules/screen_orientation/lock_orientation_callback.cc
// Upstream: content/browser/screen_orientation/screen_orientation_provider.cc
// Upstream: ui/display/display_util.cc

#ifndef MITTO_DOM_SCREEN_ORIENTATION_H_
#define MITTO_DOM_SCREEN_ORIENTATION_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/event_target.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ScreenOrientation final : public EventTarget {
 public:
  // Of a display this wide and high.
  static ScreenOrientation* Create(int width, int height);

  ScreenOrientation(int width, int height);
  ~ScreenOrientation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint16_t Angle() const;
  std::u16string Type() const;
  v8::Local<v8::Promise> Lock(const std::u16string& orientation,
                              bindings::ExceptionState& exception_state);
  void Unlock();

 private:
  const std::u16string type_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCREEN_ORIENTATION_H_
