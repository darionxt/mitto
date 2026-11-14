#include "mitto/dom/html_audio_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_audio_element.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
HTMLAudioElement* HTMLAudioElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAudioElement>(document);
}

// static
HTMLAudioElement* HTMLAudioElement::CreateForJSConstructor(
    const std::optional<std::u16string>& src) {
  HTMLAudioElement* const audio = Create(Window::Current().GetDocument());
  audio->SetAttributeWithoutValidation(u"preload", u"auto");
  if (src) {
    audio->SetAttributeWithoutValidation(u"src", *src);
  }
  return audio;
}

HTMLAudioElement::HTMLAudioElement(Document& document)
    : HTMLMediaElement(document, u"audio") {}

const bindings::WrapperTypeInfo* HTMLAudioElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAudioElement::kWrapperTypeInfo;
}

}  // namespace mitto
