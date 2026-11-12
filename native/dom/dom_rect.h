// Geometry Interfaces' DOMRect: a DOMRectReadOnly whose coordinates and sizes
// a page can set.
//
// Upstream: third_party/blink/renderer/core/geometry/dom_rect.cc

#ifndef MITTO_DOM_DOM_RECT_H_
#define MITTO_DOM_DOM_RECT_H_

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/dom_rect_read_only.h"

namespace mitto {

class DOMRect final : public DOMRectReadOnly {
 public:
  static DOMRect* Create(double x, double y, double width, double height);
  // An empty rectangle at 0, 0, as `new DOMRect()` makes, for a Known Gap that
  // gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  DOMRect(double x, double y, double width, double height);
  ~DOMRect() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  static DOMRect* FromRect(const bindings::DOMRectInit& other);
  void SetX(double x) { x_ = x; }
  void SetY(double y) { y_ = y; }
  void SetWidth(double width) { width_ = width; }
  void SetHeight(double height) { height_ = height; }
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_RECT_H_
