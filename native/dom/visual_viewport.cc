#include "mitto/dom/visual_viewport.h"

#include "mitto/bindings/v8_visual_viewport.h"

namespace mitto {

// static
VisualViewport* VisualViewport::Create(const PageStart& start) {
  return bindings::MakeGarbageCollected<VisualViewport>(start);
}

// static
bindings::ScriptWrappable* VisualViewport::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  PageStart start;
  start.viewport_width = 0;
  start.viewport_height = 0;
  return bindings::MakeForKnownGap<VisualViewport>(
      info, bindings::V8VisualViewport::kWrapperTypeInfo, start);
}

// The viewport less its scrollbars, which a page that does not overflow it
// has none of.
VisualViewport::VisualViewport(const PageStart& start)
    : width_(start.viewport_width), height_(start.viewport_height) {}

VisualViewport::~VisualViewport() = default;

const bindings::WrapperTypeInfo* VisualViewport::GetWrapperTypeInfo() const {
  return &bindings::V8VisualViewport::kWrapperTypeInfo;
}

}  // namespace mitto
