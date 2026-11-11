#include "mitto/dom/html_span_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_span_element.h"

namespace mitto {

// static
HTMLSpanElement* HTMLSpanElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLSpanElement>(document);
}

HTMLSpanElement::HTMLSpanElement(Document& document)
    : HTMLElement(document, u"span") {}

const bindings::WrapperTypeInfo* HTMLSpanElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLSpanElement::kWrapperTypeInfo;
}

}  // namespace mitto
