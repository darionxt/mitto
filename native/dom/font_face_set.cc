#include "mitto/dom/font_face_set.h"

#include "mitto/bindings/v8_font_face_set.h"

namespace mitto {

// static
FontFaceSet* FontFaceSet::Create() {
  return bindings::MakeGarbageCollected<FontFaceSet>();
}

// static
bindings::ScriptWrappable* FontFaceSet::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<FontFaceSet>(
      info, bindings::V8FontFaceSet::kWrapperTypeInfo);
}

FontFaceSet::FontFaceSet() = default;
FontFaceSet::~FontFaceSet() = default;

const bindings::WrapperTypeInfo* FontFaceSet::GetWrapperTypeInfo() const {
  return &bindings::V8FontFaceSet::kWrapperTypeInfo;
}

}  // namespace mitto
