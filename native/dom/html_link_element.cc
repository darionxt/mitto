#include "mitto/dom/html_link_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_link_element.h"

namespace mitto {

// static
HTMLLinkElement* HTMLLinkElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLinkElement>(document);
}

HTMLLinkElement::HTMLLinkElement(Document& document)
    : HTMLElement(document, u"link") {}

const bindings::WrapperTypeInfo* HTMLLinkElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLinkElement::kWrapperTypeInfo;
}

}  // namespace mitto
