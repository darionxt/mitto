#include "mitto/dom/screen.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_screen.h"
#include "mitto/dom/screen_orientation.h"

namespace mitto {
namespace {

// A display that is not HDR, as every Machine's is: the bits of each pixel
// that display::Display gives one by default.
// Upstream: ui/display/display.cc
constexpr uint32_t kColorDepth = 24;

}  // namespace

// static
Screen* Screen::Create(const PageStart& start) {
  return bindings::MakeGarbageCollected<Screen>(start);
}

Screen::Screen(const PageStart& start)
    : width_(start.screen_width),
      height_(start.screen_height),
      avail_left_(start.work_area_x),
      avail_top_(start.work_area_y),
      avail_width_(start.work_area_width),
      avail_height_(start.work_area_height) {}

Screen::~Screen() = default;

const bindings::WrapperTypeInfo* Screen::GetWrapperTypeInfo() const {
  return &bindings::V8Screen::kWrapperTypeInfo;
}

void Screen::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(orientation_);
}

int32_t Screen::AvailWidth() const {
  return avail_width_;
}

int32_t Screen::AvailHeight() const {
  return avail_height_;
}

int32_t Screen::Width() const {
  return width_;
}

int32_t Screen::Height() const {
  return height_;
}

uint32_t Screen::ColorDepth() const {
  return kColorDepth;
}

uint32_t Screen::PixelDepth() const {
  return ColorDepth();
}

int32_t Screen::AvailLeft() const {
  return avail_left_;
}

int32_t Screen::AvailTop() const {
  return avail_top_;
}

bool Screen::IsExtended() const {
  // A Machine has one display.
  return false;
}

ScreenOrientation* Screen::Orientation() {
  if (!orientation_) {
    orientation_ = ScreenOrientation::Create(width_, height_);
  }
  return orientation_.Get();
}

}  // namespace mitto
