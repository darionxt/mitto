#include "mitto/dom/dom_rect.h"

#include "mitto/bindings/v8_dom_rect.h"

namespace mitto {

// static
DOMRect* DOMRect::Create(double x, double y, double width, double height) {
  return bindings::MakeGarbageCollected<DOMRect>(x, y, width, height);
}

// static
bindings::ScriptWrappable* DOMRect::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<DOMRect>(
      info, bindings::V8DOMRect::kWrapperTypeInfo, 0.0, 0.0, 0.0, 0.0);
}

DOMRect::DOMRect(double x, double y, double width, double height)
    : DOMRectReadOnly(x, y, width, height) {}

DOMRect::~DOMRect() = default;

const bindings::WrapperTypeInfo* DOMRect::GetWrapperTypeInfo() const {
  return &bindings::V8DOMRect::kWrapperTypeInfo;
}

// static
DOMRect* DOMRect::FromRect(const bindings::DOMRectInit& other) {
  return Create(other.x, other.y, other.width, other.height);
}

}  // namespace mitto
