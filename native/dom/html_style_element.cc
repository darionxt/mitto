#include "mitto/dom/html_style_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_style_element.h"

namespace mitto {

// static
HTMLStyleElement* HTMLStyleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLStyleElement>(document);
}

HTMLStyleElement::HTMLStyleElement(Document& document)
    : HTMLElement(document, u"style") {}

const bindings::WrapperTypeInfo* HTMLStyleElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLStyleElement::kWrapperTypeInfo;
}

}  // namespace mitto
