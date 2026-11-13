#include "mitto/dom/html_image_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_image_element.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
HTMLImageElement* HTMLImageElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLImageElement>(document);
}

// static
HTMLImageElement* HTMLImageElement::CreateForJSConstructor(
    std::optional<uint32_t> width,
    std::optional<uint32_t> height) {
  HTMLImageElement* const image = Create(Window::Current().GetDocument());
  // Blink's overloads take a width before a height, so a width left
  // undefined before a height converts to 0.
  if (height && !width) {
    width = 0;
  }
  if (width) {
    image->SetWidth(*width);
  }
  if (height) {
    image->SetHeight(*height);
  }
  return image;
}

HTMLImageElement::HTMLImageElement(Document& document)
    : HTMLElement(document, u"img") {}

const bindings::WrapperTypeInfo* HTMLImageElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLImageElement::kWrapperTypeInfo;
}

uint32_t HTMLImageElement::Width() const {
  const std::u16string* value = FindAttribute(u"width");
  return value ? ParseHTMLNonNegativeInteger(*value).value_or(0) : 0;
}

void HTMLImageElement::SetWidth(uint32_t value) {
  SetReflectedUnsignedIntegerAttribute(u"width", value);
}

uint32_t HTMLImageElement::Height() const {
  const std::u16string* value = FindAttribute(u"height");
  return value ? ParseHTMLNonNegativeInteger(*value).value_or(0) : 0;
}

void HTMLImageElement::SetHeight(uint32_t value) {
  SetReflectedUnsignedIntegerAttribute(u"height", value);
}

}  // namespace mitto
