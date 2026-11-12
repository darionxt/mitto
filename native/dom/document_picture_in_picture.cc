#include "mitto/dom/document_picture_in_picture.h"

#include "mitto/bindings/v8_document_picture_in_picture.h"

namespace mitto {

// static
DocumentPictureInPicture* DocumentPictureInPicture::Create() {
  return bindings::MakeGarbageCollected<DocumentPictureInPicture>();
}

// static
bindings::ScriptWrappable* DocumentPictureInPicture::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<DocumentPictureInPicture>(
      info, bindings::V8DocumentPictureInPicture::kWrapperTypeInfo);
}

DocumentPictureInPicture::DocumentPictureInPicture() = default;
DocumentPictureInPicture::~DocumentPictureInPicture() = default;

const bindings::WrapperTypeInfo* DocumentPictureInPicture::GetWrapperTypeInfo()
    const {
  return &bindings::V8DocumentPictureInPicture::kWrapperTypeInfo;
}

}  // namespace mitto
