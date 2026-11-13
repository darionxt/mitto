#include "mitto/dom/html_area_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_area_element.h"

namespace mitto {

// static
HTMLAreaElement* HTMLAreaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAreaElement>(document);
}

HTMLAreaElement::HTMLAreaElement(Document& document)
    : HTMLElement(document, u"area") {}

const bindings::WrapperTypeInfo* HTMLAreaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAreaElement::kWrapperTypeInfo;
}

std::u16string HTMLAreaElement::Href() const {
  return ReflectedURLAttribute(u"href");
}

void HTMLAreaElement::SetHref(const std::u16string& value) {
  SetAttributeWithoutValidation(u"href", value);
}

}  // namespace mitto
