// Document Picture-in-Picture's DocumentPictureInPicture, the window's
// documentPictureInPicture, which has opened no window. requestWindow() is a
// Known Gap.
//
// Upstream: third_party/blink/renderer/modules/document_picture_in_picture/document_picture_in_picture.cc

#ifndef MITTO_DOM_DOCUMENT_PICTURE_IN_PICTURE_H_
#define MITTO_DOM_DOCUMENT_PICTURE_IN_PICTURE_H_

#include "mitto/dom/event_target.h"
#include "mitto/dom/window.h"

namespace mitto {

class DocumentPictureInPicture final : public EventTarget {
 public:
  static DocumentPictureInPicture* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  DocumentPictureInPicture();
  ~DocumentPictureInPicture() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  // Null, which Blink's gives until requestWindow() has opened one, though
  // the IDL does not make it nullable.
  mitto::Window* Window() const { return nullptr; }
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_PICTURE_IN_PICTURE_H_
