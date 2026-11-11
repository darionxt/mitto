#include "mitto/dom/html_paragraph_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_paragraph_element.h"

namespace mitto {

// static
HTMLParagraphElement* HTMLParagraphElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLParagraphElement>(document);
}

HTMLParagraphElement::HTMLParagraphElement(Document& document)
    : HTMLElement(document, u"p") {}

const bindings::WrapperTypeInfo* HTMLParagraphElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLParagraphElement::kWrapperTypeInfo;
}

}  // namespace mitto
