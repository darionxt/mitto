#include "mitto/dom/html_olist_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_olist_element.h"

namespace mitto {

// static
HTMLOListElement* HTMLOListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLOListElement>(document);
}

HTMLOListElement::HTMLOListElement(Document& document)
    : HTMLElement(document, u"ol") {}

const bindings::WrapperTypeInfo* HTMLOListElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLOListElement::kWrapperTypeInfo;
}

}  // namespace mitto
