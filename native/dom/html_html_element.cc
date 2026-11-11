#include "mitto/dom/html_html_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_html_element.h"

namespace mitto {

// static
HTMLHtmlElement* HTMLHtmlElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLHtmlElement>(document);
}

HTMLHtmlElement::HTMLHtmlElement(Document& document)
    : HTMLElement(document, u"html") {}

const bindings::WrapperTypeInfo* HTMLHtmlElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLHtmlElement::kWrapperTypeInfo;
}

}  // namespace mitto
