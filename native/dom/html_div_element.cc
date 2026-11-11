#include "mitto/dom/html_div_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_div_element.h"

namespace mitto {

// static
HTMLDivElement* HTMLDivElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDivElement>(document);
}

HTMLDivElement::HTMLDivElement(Document& document)
    : HTMLElement(document, u"div") {}

const bindings::WrapperTypeInfo* HTMLDivElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDivElement::kWrapperTypeInfo;
}

}  // namespace mitto
