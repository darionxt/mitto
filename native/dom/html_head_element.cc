#include "mitto/dom/html_head_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_head_element.h"

namespace mitto {

// static
HTMLHeadElement* HTMLHeadElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLHeadElement>(document);
}

HTMLHeadElement::HTMLHeadElement(Document& document)
    : HTMLElement(document, u"head") {}

const bindings::WrapperTypeInfo* HTMLHeadElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLHeadElement::kWrapperTypeInfo;
}

}  // namespace mitto
