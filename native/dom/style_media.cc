#include "mitto/dom/style_media.h"

#include "mitto/bindings/v8_style_media.h"

namespace mitto {

// static
StyleMedia* StyleMedia::Create() {
  return bindings::MakeGarbageCollected<StyleMedia>();
}

// static
bindings::ScriptWrappable* StyleMedia::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<StyleMedia>(
      info, bindings::V8StyleMedia::kWrapperTypeInfo);
}

StyleMedia::StyleMedia() = default;
StyleMedia::~StyleMedia() = default;

const bindings::WrapperTypeInfo* StyleMedia::GetWrapperTypeInfo() const {
  return &bindings::V8StyleMedia::kWrapperTypeInfo;
}

}  // namespace mitto
