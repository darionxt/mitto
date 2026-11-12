// CSSOM View's old StyleMedia, the window's styleMedia: the media type of a
// tab's frame, "screen". matchMedium() is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/css/style_media.cc

#ifndef MITTO_DOM_STYLE_MEDIA_H_
#define MITTO_DOM_STYLE_MEDIA_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class StyleMedia final : public bindings::ScriptWrappable {
 public:
  static StyleMedia* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  StyleMedia();
  ~StyleMedia() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Type() const { return u"screen"; }
};

}  // namespace mitto

#endif  // MITTO_DOM_STYLE_MEDIA_H_
