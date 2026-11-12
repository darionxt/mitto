// Geometry Interfaces' DOMRectReadOnly: a rectangle, whose edges a page reads
// as Blink computes them, NaN where a coordinate or a size is.
//
// Upstream: third_party/blink/renderer/core/geometry/dom_rect_read_only.cc
// Upstream: third_party/blink/renderer/core/geometry/geometry_util.h

#ifndef MITTO_DOM_DOM_RECT_READ_ONLY_H_
#define MITTO_DOM_DOM_RECT_READ_ONLY_H_

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class DOMRectReadOnly : public bindings::ScriptWrappable {
 public:
  static DOMRectReadOnly* Create(double x,
                                 double y,
                                 double width,
                                 double height);
  // An empty rectangle at 0, 0, as `new DOMRectReadOnly()` makes, for a Known
  // Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  DOMRectReadOnly(double x, double y, double width, double height);
  ~DOMRectReadOnly() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  static DOMRectReadOnly* FromRect(const bindings::DOMRectInit& other);
  double X() const { return x_; }
  double Y() const { return y_; }
  double Width() const { return width_; }
  double Height() const { return height_; }
  double Top() const;
  double Right() const;
  double Bottom() const;
  double Left() const;

 protected:
  double x_;
  double y_;
  double width_;
  double height_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_RECT_READ_ONLY_H_
