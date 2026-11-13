// HTMLMediaElement, of the audio and video elements. Playing media stays out
// of the Runtime, so what it has beyond its reflected attributes is what
// needs no media.
//
// Upstream: third_party/blink/renderer/core/html/media/html_media_element.h
// Upstream: third_party/blink/renderer/core/html/media/html_media_element.cc

#ifndef MITTO_DOM_HTML_MEDIA_ELEMENT_H_
#define MITTO_DOM_HTML_MEDIA_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLMediaElement : public HTMLElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. preload, as Blink's PreloadType() reads
  // it: "none", "metadata", or "auto" for an empty value, and "metadata" for
  // a missing or another one, on a connection that is not cellular.
  std::u16string Preload() const;
  void SetPreload(const std::u16string& value);

 protected:
  HTMLMediaElement(Document& document, const std::u16string& local_name);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_MEDIA_ELEMENT_H_
