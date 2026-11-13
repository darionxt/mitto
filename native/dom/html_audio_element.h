// The audio element, which new Audio() makes too. Playing media stays out of
// the Runtime, so its members beyond its attributes are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/html/media/html_audio_element.h
// Upstream: third_party/blink/renderer/core/html/media/html_audio_element.cc

#ifndef MITTO_DOM_HTML_AUDIO_ELEMENT_H_
#define MITTO_DOM_HTML_AUDIO_ELEMENT_H_

#include <optional>
#include <string>

#include "mitto/dom/html_media_element.h"

namespace mitto {

class HTMLAudioElement final : public HTMLMediaElement {
 public:
  static HTMLAudioElement* Create(Document& document);
  // new Audio(src), in the current window's document: preload is "auto",
  // and src the URL given, as Blink's CreateForJSConstructor() sets them.
  static HTMLAudioElement* CreateForJSConstructor(
      const std::optional<std::u16string>& src);

  explicit HTMLAudioElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_AUDIO_ELEMENT_H_
