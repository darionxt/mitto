// CSSOM View's Screen, a window's `screen`: the Machine's primary display, its
// work area and its orientation, in device-independent pixels. Its event
// handler attribute is a Known Gap, and the display never changes, so no
// change event comes.
//
// Upstream: third_party/blink/renderer/core/frame/screen.cc
// Upstream: ui/display/display_util.cc

#ifndef MITTO_DOM_SCREEN_H_
#define MITTO_DOM_SCREEN_H_

#include <stdint.h>

#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ScreenOrientation;

class Screen final : public EventTarget {
 public:
  static Screen* Create(const PageStart& start);

  explicit Screen(const PageStart& start);
  ~Screen() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  int32_t AvailWidth() const;
  int32_t AvailHeight() const;
  int32_t Width() const;
  int32_t Height() const;
  uint32_t ColorDepth() const;
  uint32_t PixelDepth() const;
  int32_t AvailLeft() const;
  int32_t AvailTop() const;
  bool IsExtended() const;
  ScreenOrientation* Orientation();

 private:
  const int width_;
  const int height_;
  const int avail_left_;
  const int avail_top_;
  const int avail_width_;
  const int avail_height_;
  cppgc::Member<ScreenOrientation> orientation_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCREEN_H_
