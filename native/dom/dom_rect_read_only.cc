#include "mitto/dom/dom_rect_read_only.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mitto/bindings/v8_dom_rect_read_only.h"

namespace mitto {
namespace {

// Blink's NanSafeMin() and NanSafeMax(): NaN if either is.
double NanSafeMin(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::min(a, b);
}

double NanSafeMax(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::max(a, b);
}

}  // namespace

// static
DOMRectReadOnly* DOMRectReadOnly::Create(double x,
                                         double y,
                                         double width,
                                         double height) {
  return bindings::MakeGarbageCollected<DOMRectReadOnly>(x, y, width, height);
}

// static
bindings::ScriptWrappable* DOMRectReadOnly::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<DOMRectReadOnly>(
      info, bindings::V8DOMRectReadOnly::kWrapperTypeInfo, 0.0, 0.0, 0.0, 0.0);
}

DOMRectReadOnly::DOMRectReadOnly(double x,
                                 double y,
                                 double width,
                                 double height)
    : x_(x), y_(y), width_(width), height_(height) {}

DOMRectReadOnly::~DOMRectReadOnly() = default;

const bindings::WrapperTypeInfo* DOMRectReadOnly::GetWrapperTypeInfo() const {
  return &bindings::V8DOMRectReadOnly::kWrapperTypeInfo;
}

// static
DOMRectReadOnly* DOMRectReadOnly::FromRect(const bindings::DOMRectInit& other) {
  return Create(other.x, other.y, other.width, other.height);
}

double DOMRectReadOnly::Top() const {
  return NanSafeMin(y_, y_ + height_);
}

double DOMRectReadOnly::Right() const {
  return NanSafeMax(x_, x_ + width_);
}

double DOMRectReadOnly::Bottom() const {
  return NanSafeMax(y_, y_ + height_);
}

double DOMRectReadOnly::Left() const {
  return NanSafeMin(x_, x_ + width_);
}

}  // namespace mitto
