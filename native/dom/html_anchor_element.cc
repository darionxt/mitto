#include "mitto/dom/html_anchor_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_anchor_element.h"

namespace mitto {

// static
HTMLAnchorElement* HTMLAnchorElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAnchorElement>(document);
}

HTMLAnchorElement::HTMLAnchorElement(Document& document)
    : HTMLElement(document, u"a") {}

const bindings::WrapperTypeInfo* HTMLAnchorElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAnchorElement::kWrapperTypeInfo;
}

std::u16string HTMLAnchorElement::Href() const {
  return ReflectedURLAttribute(u"href");
}

void HTMLAnchorElement::SetHref(const std::u16string& value) {
  SetAttributeWithoutValidation(u"href", value);
}

}  // namespace mitto
