#include "mitto/dom/html_br_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_br_element.h"

namespace mitto {

// static
HTMLBRElement* HTMLBRElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLBRElement>(document);
}

HTMLBRElement::HTMLBRElement(Document& document)
    : HTMLElement(document, u"br") {}

const bindings::WrapperTypeInfo* HTMLBRElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLBRElement::kWrapperTypeInfo;
}

}  // namespace mitto
