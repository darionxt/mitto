#include "mitto/dom/bar_prop.h"

#include "mitto/bindings/v8_bar_prop.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
BarProp* BarProp::Create(Window& window) {
  return bindings::MakeGarbageCollected<BarProp>(&window);
}

// static
bindings::ScriptWrappable* BarProp::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<BarProp>(
      info, bindings::V8BarProp::kWrapperTypeInfo, nullptr);
}

BarProp::BarProp(Window* window) : window_(window) {}
BarProp::~BarProp() = default;

const bindings::WrapperTypeInfo* BarProp::GetWrapperTypeInfo() const {
  return &bindings::V8BarProp::kWrapperTypeInfo;
}

void BarProp::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

bool BarProp::Visible() const {
  const Window* const window = window_.Get();
  return window && window->HasContext();
}

}  // namespace mitto
