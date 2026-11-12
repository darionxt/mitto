// CSS Font Loading's FontFaceSet, document.fonts: a document that loads no
// web font, as a Page's does until it has fonts (roadmap, M9), has loaded
// its fonts. Its other members, the set's among them, are Known Gaps, and it
// holds no face.
//
// Upstream: third_party/blink/renderer/core/css/font_face_set.cc

#ifndef MITTO_DOM_FONT_FACE_SET_H_
#define MITTO_DOM_FONT_FACE_SET_H_

#include <string>

#include "mitto/dom/event_target.h"

namespace mitto {

class FontFaceSet final : public EventTarget {
 public:
  static FontFaceSet* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  FontFaceSet();
  ~FontFaceSet() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Status() const { return u"loaded"; }
};

}  // namespace mitto

#endif  // MITTO_DOM_FONT_FACE_SET_H_
