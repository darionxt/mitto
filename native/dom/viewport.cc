#include "mitto/dom/viewport.h"

#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_viewport.h"
#include "mitto/dom/dom_rect.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
Viewport* Viewport::Create(const PageStart& start) {
  return bindings::MakeGarbageCollected<Viewport>(start);
}

// static
bindings::ScriptWrappable* Viewport::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  PageStart start;
  start.viewport_width = 0;
  start.viewport_height = 0;
  return bindings::MakeForKnownGap<Viewport>(
      info, bindings::V8Viewport::kWrapperTypeInfo, start);
}

Viewport::Viewport(const PageStart& start)
    : width_(start.viewport_width), height_(start.viewport_height) {}

Viewport::~Viewport() = default;

const bindings::WrapperTypeInfo* Viewport::GetWrapperTypeInfo() const {
  return &bindings::V8Viewport::kWrapperTypeInfo;
}

// The widget's one segment, its whole size in DIPs at the page's zoom of 1,
// which DOMViewport::segments() scales into the page's coordinates.
v8::Local<v8::Value> Viewport::Segments() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return FrozenArray(
      isolate,
      {bindings::ToV8(isolate, DOMRect::Create(0, 0, width_, height_))});
}

}  // namespace mitto
